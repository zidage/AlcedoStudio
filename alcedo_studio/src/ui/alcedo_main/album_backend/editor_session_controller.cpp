//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "edit/frame_presentation_types.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "type/hash_type.hpp"
#include "ui/alcedo_main/album_backend/album_catalog.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::ui {
namespace {

constexpr auto   kFilmstripCollapsedKey          = "editor/filmstripCollapsed";
constexpr auto   kFilmstripExpandedHeightKey     = "editor/filmstripExpandedHeight";
constexpr auto   kActiveAdjustmentPanelKey       = "editor/activeAdjustmentPanel";
// Floor matches the default expanded dock (minimum scale = current proportion).
// Ceiling is intentionally high: QML clamps live drag to ≤ 50% of the window.
constexpr double kFilmstripExpandedHeightMin     = 128.0;
constexpr double kFilmstripExpandedHeightMax     = 4096.0;
constexpr double kFilmstripExpandedHeightDefault = 128.0;

}  // namespace

EditorSessionController::EditorSessionController(QObject* parent)
    : EditorSessionController(nullptr, parent) {}

EditorSessionController::EditorSessionController(alcedo::IEditorSessionBackend* session_backend,
                                                 QObject*                       parent)
    : QObject(parent), session_backend_(session_backend), actions_(this) {
  connect(&actions_, &EditorActionAvailabilityModel::AvailabilityChanged, this,
          &EditorSessionController::ActionAvailabilityChanged);
  scope_controller_ = std::make_unique<EditorScopeController>(this);
  connect(scope_controller_.get(), &EditorScopeController::FrameRequested, this, [this]() {
    if (!session_backend_ || !has_image() ||
        session_backend_->state() != alcedo::EditorSessionState::Interactive) {
      return;
    }
    std::optional<alcedo::ViewportRenderRegion> region;
    if (auto* sink = presentation_frame_sink()) {
      region = sink->GetViewportRenderRegion();
    }
    session_backend_->RequestViewChange(alcedo::EditorRenderReason::ScopeRefresh,
                                        std::move(region));
  });
  scope_controller_->SetImageIdentity(image_id(), SessionEpoch());
  LoadFilmstripUiPrefs();
  LoadDesktopUiPrefs();
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(active_adjustment_panel_ ==
                                               QLatin1String("geometry"));
    SyncIdentityFromBackend();
    ApplyActionAvailability();
  }
  InstallBackendNotifier();
}

EditorSessionController::~EditorSessionController() {
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
    session_backend_->SetResultObserver({});
    session_backend_->SetActionAvailabilityObserver({});
  }
}

void EditorSessionController::InstallBackendNotifier() {
  if (!session_backend_) {
    return;
  }
  QPointer<EditorSessionController> self(this);
  session_backend_->SetChangeNotifier([self] {
    if (!self) {
      return;
    }
    if (QThread::currentThread() == self->thread()) {
      self->OnBackendChanged();
      return;
    }
    QMetaObject::invokeMethod(
        self,
        [self] {
          if (self) {
            self->OnBackendChanged();
          }
        },
        Qt::QueuedConnection);
  });
  // Phase 7A R4: install the session result observer so async SaveFinished /
  // Failed / Rejected outcomes reach QML as correlated HistoryOperationFinished
  // events. ChangeNotifier alone only mirrors state; it does not carry action
  // identity or the exact backend message for the initiating create/branch/checkout.
  session_backend_->SetResultObserver([self](const alcedo::EditorSessionResult& result) {
    if (!self) {
      return;
    }
    if (QThread::currentThread() == self->thread()) {
      self->OnBackendSessionResult(result);
      return;
    }
    QMetaObject::invokeMethod(
        self,
        [self, result] {
          if (self) {
            self->OnBackendSessionResult(result);
          }
        },
        Qt::QueuedConnection);
  });
  session_backend_->SetActionAvailabilityObserver(
      [self](const alcedo::EditorActionAvailability& availability) {
        if (!self) {
          return;
        }
        if (QThread::currentThread() == self->thread()) {
          self->actions_.Apply(availability);
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, availability] {
              if (self) {
                self->actions_.Apply(availability);
              }
            },
            Qt::QueuedConnection);
      });
}

void EditorSessionController::SetSessionBackend(alcedo::IEditorSessionBackend* session_backend) {
  if (session_backend_ == session_backend) {
    return;
  }
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
    session_backend_->SetResultObserver({});
    session_backend_->SetActionAvailabilityObserver({});
  }
  session_backend_ = session_backend;
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(active_adjustment_panel_ ==
                                               QLatin1String("geometry"));
    InstallBackendNotifier();
    SyncIdentityFromBackend();
    ApplyActionAvailability();
  } else {
    actions_.Apply({});
  }
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(image_id(), SessionEpoch());
  }
}

void EditorSessionController::SetInteractionPolicy(
    InteractionPolicyController* interaction_policy) {
  if (interaction_policy_ == interaction_policy) {
    return;
  }
  if (interaction_policy_) {
    QObject::disconnect(interaction_policy_connection_);
    interaction_policy_connection_ = {};
  }
  interaction_policy_ = interaction_policy;
  if (interaction_policy_) {
    interaction_policy_connection_ =
        connect(interaction_policy_, &InteractionPolicyController::PolicyChanged, this,
                &EditorSessionController::SyncBackgroundActionRestrictions);
    SyncBackgroundActionRestrictions();
  }
}

void EditorSessionController::SetCopiedPackageAvailable(bool available) {
  if (!session_backend_) {
    return;
  }
  session_backend_->SetCopiedPackageAvailable(available);
  ApplyActionAvailability();
}

void EditorSessionController::SetAlbumCatalog(IAlbumCatalog* album_catalog) {
  album_catalog_ = album_catalog;
}

void EditorSessionController::SyncBackgroundActionRestrictions() {
  if (!session_backend_ || !interaction_policy_) {
    return;
  }
  alcedo::EditorBackgroundActionRestrictions restrictions;
  restrictions.blocks_select_image = !interaction_policy_->CanSelectEditorImage();
  restrictions.blocks_paste        = !interaction_policy_->CanPasteAdjustments();
  restrictions.blocks_checkout     = !interaction_policy_->CanCheckoutVersion();
  restrictions.blocks_workspace    = !interaction_policy_->CanSwitchWorkspace();
  session_backend_->SetBackgroundActionRestrictions(restrictions);
  ApplyActionAvailability();
}

void EditorSessionController::ApplyActionAvailability() {
  if (!session_backend_) {
    actions_.Apply({});
    return;
  }
  actions_.Apply(session_backend_->action_availability());
}

void EditorSessionController::OnBackendChanged() {
  if (!session_backend_) {
    return;
  }
  SyncIdentityFromBackend();
  SyncViewportIdentity();
  ApplyActionAvailability();

  // Phase 6C-7: keep the cached snapshot map warm on every backend change, but
  // only emit AdjustmentSnapshotChanged when not suppressed. Interactive
  // submitPatch suppresses the emit so each pointer move does not re-enter
  // EditorAdjustmentStack.loadFromSnapshot (QML signal storm → GUI stall when
  // switching sliders rapidly while history/render also touch the pipeline).
  const auto render_snapshot = session_backend_->adjustment_snapshot();
  auto       panel_snapshot  = BuildSnapshotMap(render_snapshot);
  if (panel_snapshot != adjustment_snapshot_) {
    adjustment_snapshot_ = std::move(panel_snapshot);
    if (!suppress_snapshot_publish_) {
      emit AdjustmentSnapshotChanged();
      SyncAlbumHdrFlagFromSnapshot();
    }
  }
  SyncViewportDisplayConfig();
  emit       StateChanged();
  // Phase 7A R2: emit the dedicated history signal only when the backend's
  // monotonic history_revision advances. Render-busy, frame-ready, preview,
  // progress, viewport, and task-detail notifications leave the revision
  // unchanged, so EditorHistoryModel no longer projects on every renderer
  // event. Settled commits, head moves, Version ref changes, image open/close,
  // and recovery each bump the revision once and trigger exactly one
  // projection.
  const auto history_revision = session_backend_->history_revision();
  if (history_revision != last_history_revision_) {
    last_history_revision_ = history_revision;
    emit HistoryChanged();
  }
}

auto EditorSessionController::active() const -> bool {
  // Workspace membership: true for empty editor as well as loading/interactive.
  // Backend NoImage is still an active editor session until Close/Finalize.
  return active_;
}

auto EditorSessionController::has_image() const -> bool {
  if (session_backend_) {
    // The backend may keep the current identity while an asynchronous close
    // checkpoint finishes.  The workspace route has already left the editor
    // at that point, so do not expose the stale image to QML controls.
    return active_ && session_backend_->has_image();
  }
  return active_ && element_id_ > 0 && image_id_ > 0;
}

auto EditorSessionController::has_pending_recovery() const -> bool {
  return session_backend_ != nullptr && session_backend_->has_pending_recovery();
}

auto EditorSessionController::element_id() const -> uint {
  if (session_backend_) {
    return session_backend_->identity().element_id;
  }
  return element_id_;
}

auto EditorSessionController::image_id() const -> uint {
  if (session_backend_) {
    return session_backend_->identity().image_id;
  }
  return image_id_;
}

auto EditorSessionController::SessionEpoch() const -> qulonglong {
  if (session_backend_) {
    return static_cast<qulonglong>(session_backend_->active_image_load_request().value);
  }
  return session_generation_;
}

auto EditorSessionController::viewport_identity_key() const -> QString {
  return QStringLiteral("%1:%2:%3").arg(image_id()).arg(SessionEpoch()).arg(active_ ? 1 : 0);
}

auto EditorSessionController::session_state() const -> alcedo::EditorSessionState {
  if (session_backend_) {
    return session_backend_->state();
  }
  return session_state_;
}

auto EditorSessionController::session_state_name() const -> QString {
  return QString::fromUtf8(alcedo::EditorSessionStateName(session_state()));
}

void EditorSessionController::SyncIdentityFromBackend() {
  if (!session_backend_) {
    return;
  }
  const auto id = session_backend_->identity();
  element_id_   = id.element_id;
  image_id_     = id.image_id;
  session_generation_ =
      static_cast<qulonglong>(session_backend_->active_image_load_request().value);
  session_state_ = session_backend_->state();
  // active_ is workspace membership owned by Open/Close/Finalize, not by
  // backend NoImage vs Loading (empty editor remains active).
}

void EditorSessionController::ApplyOpenLocal(uint elementId, uint imageId) {
  active_        = true;
  element_id_    = elementId;
  image_id_      = imageId;
  session_state_ = (elementId > 0 && imageId > 0) ? alcedo::EditorSessionState::Loading
                                                  : alcedo::EditorSessionState::NoImage;
  if (elementId > 0 && imageId > 0 &&
      (last_element_id_ != elementId || last_image_id_ != imageId)) {
    last_element_id_ = elementId;
    last_image_id_   = imageId;
    emit LastEditedImageChanged();
  }
  ++session_generation_;
}

void EditorSessionController::ApplyCloseLocal() {
  if (!active_ && element_id_ == 0 && image_id_ == 0 &&
      session_state_ == alcedo::EditorSessionState::NoImage) {
    return;
  }
  active_        = false;
  element_id_    = 0;
  image_id_      = 0;
  session_state_ = alcedo::EditorSessionState::NoImage;
}

void EditorSessionController::SyncViewportIdentity() {
  qulonglong target_image_id   = static_cast<qulonglong>(image_id());
  qulonglong target_generation = SessionEpoch();
  if (session_backend_) {
    if (const auto pending = session_backend_->pending_presentation_target()) {
      target_image_id   = static_cast<qulonglong>(pending->image_id);
      target_generation = static_cast<qulonglong>(pending->image_load_request.value);
    } else if (session_state() == alcedo::EditorSessionState::Saving) {
      // Preserve the Open() pre-stamp until the backend publishes a pending target.
      if (auto* item =
              qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
        if (item->sessionEpoch() > target_generation) {
          target_image_id   = item->imageIdentity();
          target_generation = item->sessionEpoch();
        }
      }
    }
  }
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(target_image_id, target_generation);
  }
  if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
    const bool presentation_active =
        target_image_id > 0 &&
        (has_image() || (session_backend_ && session_backend_->pending_presentation_target()));
    if (presentation_active) {
      item->setImageIdentity(target_image_id);
      item->setSessionEpoch(target_generation);
    }
  }
}

void EditorSessionController::Open(uint elementId, uint imageId) {
  // Remember the last real image so re-entering the editor from the library can
  // restore it (Phase 4A-Fix). Close/Finalize never touch this; only an explicit
  // clearLastEditedImage() (delete / project switch) forgets it.
  if (elementId > 0 && imageId > 0 &&
      (last_element_id_ != elementId || last_image_id_ != imageId)) {
    last_element_id_ = elementId;
    last_image_id_   = imageId;
    emit LastEditedImageChanged();
  }

  if (session_backend_) {
    // Stamp the viewport before Open/Switch can schedule its worker. Otherwise
    // EnsureSize may publish a generation-0 target and the guaranteed first
    // frame races permanently ahead of the controller's post-call mirror.
    if (elementId > 0 && imageId > 0) {
      const bool same_image = session_backend_->has_image() &&
                              session_backend_->identity().element_id == elementId &&
                              session_backend_->identity().image_id == imageId;
      bool skip_prestamp = false;
      if (session_state() == alcedo::EditorSessionState::Saving) {
        if (auto* item =
                qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
          skip_prestamp =
              item->sessionEpoch() >
              static_cast<qulonglong>(session_backend_->active_image_load_request().value);
        }
      }
      if (!skip_prestamp) {
        const qulonglong presentation_image_id   = static_cast<qulonglong>(imageId);
        const qulonglong presentation_generation = static_cast<qulonglong>(
            session_backend_->active_image_load_request().value + (same_image ? 0 : 1));
        if (scope_controller_) {
          scope_controller_->SetImageIdentity(presentation_image_id, presentation_generation);
        }
        if (auto* item =
                qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
          item->setImageIdentity(presentation_image_id);
          item->setSessionEpoch(presentation_generation);
        }
      }
    }
    if (elementId == 0 || imageId == 0) {
      if (scope_controller_) {
        scope_controller_->SetImageIdentity(0, 0);
      }
      // Empty editor is an explicit persisted close, not a fake image open.
      if (auto* item =
              qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
        item->suspendPresentation();
      }
      session_backend_->Close(/*persist_changes=*/true);
    } else if (session_backend_->has_image() &&
               (session_backend_->identity().element_id != elementId ||
                session_backend_->identity().image_id != imageId)) {
      session_backend_->Switch(elementId, imageId);
    } else {
      session_backend_->Open(elementId, imageId);
    }
    SyncIdentityFromBackend();
    active_ = true;
  } else {
    ApplyOpenLocal(elementId, imageId);
  }

  SyncViewportIdentity();
  emit StateChanged();
}

void EditorSessionController::CheckoutVersion(const QString& versionId) {
  const QString action = QStringLiteral("checkoutVersion");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"),
                           versionId);
    return;
  }
  const QString trimmed = versionId.trimmed();
  if (trimmed.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Version id is required"));
    return;
  }
  try {
    const auto version_id = alcedo::Hash128::FromString(trimmed.toStdString());
    auto       result     = session_backend_->CheckoutVersion(version_id);
    SyncIdentityFromBackend();
    emit StateChanged();
    PublishHistoryInvokableReturn(action, result, trimmed);
  } catch (const std::exception& ex) {
    const QString message = QString::fromUtf8(ex.what()).trimmed();
    PublishHistoryRejected(
        action, message.isEmpty() ? QStringLiteral("Invalid Version id") : message, trimmed);
  }
}

void EditorSessionController::CreateRootVersion(const QString& displayName) {
  const QString action = QStringLiteral("createRootVersion");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  const QString name = displayName.trimmed();
  if (name.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Version name is required"));
    return;
  }
  auto result = session_backend_->CreateRootVersion(name.toStdString());
  SyncIdentityFromBackend();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::BranchFromCommit(const QString& commitId,
                                               const QString& displayName) {
  const QString action = QStringLiteral("branchFromCommit");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"),
                           commitId);
    return;
  }
  const QString trimmed_commit = commitId.trimmed();
  const QString name           = displayName.trimmed();
  if (trimmed_commit.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Commit id is required"));
    return;
  }
  if (name.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Branch name is required"), trimmed_commit);
    return;
  }
  try {
    const auto id     = alcedo::Hash128::FromString(trimmed_commit.toStdString());
    auto       result = session_backend_->BranchFromCommit(id, name.toStdString());
    SyncIdentityFromBackend();
    emit StateChanged();
    PublishHistoryInvokableReturn(action, result, trimmed_commit);
  } catch (const std::exception& ex) {
    const QString message = QString::fromUtf8(ex.what()).trimmed();
    PublishHistoryRejected(
        action, message.isEmpty() ? QStringLiteral("Invalid commit id") : message, trimmed_commit);
  }
}

void EditorSessionController::RetrySave() {
  const QString action = QStringLiteral("retrySave");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->RetrySave();
  SyncIdentityFromBackend();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::DiscardAndContinue() {
  const QString action = QStringLiteral("discardAndContinue");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->DiscardAndContinue();
  SyncIdentityFromBackend();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::Discard() {
  const QString action = QStringLiteral("discard");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->Discard();
  SyncIdentityFromBackend();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::CancelPendingNavigation() {
  const QString action = QStringLiteral("cancelPendingNavigation");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->CancelPendingNavigation();
  SyncIdentityFromBackend();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::RenameVersion(const QString& versionId, const QString& displayName) {
  const QString action = QStringLiteral("renameVersion");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"),
                           versionId);
    return;
  }
  const QString trimmed = versionId.trimmed();
  const QString name    = displayName.trimmed();
  if (trimmed.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Version id is required"));
    return;
  }
  if (name.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Version name is required"), trimmed);
    return;
  }
  try {
    const auto id     = alcedo::Hash128::FromString(trimmed.toStdString());
    auto       result = session_backend_->RenameVersion(id, name.toStdString());
    emit       StateChanged();
    PublishHistoryInvokableReturn(action, result, trimmed);
  } catch (const std::exception& ex) {
    const QString message = QString::fromUtf8(ex.what()).trimmed();
    PublishHistoryRejected(
        action, message.isEmpty() ? QStringLiteral("Invalid Version id") : message, trimmed);
  }
}

void EditorSessionController::RemoveVersion(const QString& versionId) {
  const QString action = QStringLiteral("removeVersion");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"),
                           versionId);
    return;
  }
  const QString trimmed = versionId.trimmed();
  if (trimmed.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Version id is required"));
    return;
  }
  try {
    const auto id     = alcedo::Hash128::FromString(trimmed.toStdString());
    auto       result = session_backend_->RemoveVersion(id);
    emit       StateChanged();
    PublishHistoryInvokableReturn(action, result, trimmed);
  } catch (const std::exception& ex) {
    const QString message = QString::fromUtf8(ex.what()).trimmed();
    PublishHistoryRejected(
        action, message.isEmpty() ? QStringLiteral("Invalid Version id") : message, trimmed);
  }
}

void EditorSessionController::Undo() {
  const QString action = QStringLiteral("undo");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->Undo();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

void EditorSessionController::Redo() {
  const QString action = QStringLiteral("redo");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"));
    return;
  }
  auto result = session_backend_->Redo();
  emit StateChanged();
  PublishHistoryInvokableReturn(action, result);
}

auto EditorSessionController::SubmitAddColorGrade(const alcedo::NodeId& before_node_id,
                                                  const alcedo::NodeId& new_id)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) {
    alcedo::EditorSessionResult result;
    result.kind    = alcedo::EditorSessionResultKind::Rejected;
    result.state   = session_state();
    result.message = "Editor session backend is unavailable";
    return result;
  }
  return session_backend_->AddColorGrade(before_node_id, new_id);
}

auto EditorSessionController::SubmitRemoveColorGrade(const alcedo::NodeId& node_id)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) {
    alcedo::EditorSessionResult result;
    result.kind    = alcedo::EditorSessionResultKind::Rejected;
    result.state   = session_state();
    result.message = "Editor session backend is unavailable";
    return result;
  }
  return session_backend_->RemoveColorGrade(node_id);
}

auto EditorSessionController::SubmitRenameColorGrade(const alcedo::NodeId& node_id,
                                                     std::string           display_name)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) {
    alcedo::EditorSessionResult result;
    result.kind    = alcedo::EditorSessionResultKind::Rejected;
    result.state   = session_state();
    result.message = "Editor session backend is unavailable";
    return result;
  }
  return session_backend_->RenameColorGrade(node_id, std::move(display_name));
}

auto EditorSessionController::SubmitReconnectColorGrade(const alcedo::NodeId& node_id,
                                                        const alcedo::NodeId& new_predecessor_id,
                                                        const alcedo::NodeId& new_successor_id)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) {
    alcedo::EditorSessionResult result;
    result.kind    = alcedo::EditorSessionResultKind::Rejected;
    result.state   = session_state();
    result.message = "Editor session backend is unavailable";
    return result;
  }
  return session_backend_->ReconnectColorGrade(node_id, new_predecessor_id, new_successor_id);
}

auto EditorSessionController::SubmitNodeGraphTopologyEdit(
    const alcedo::NodeGraphTopologyChange& change) -> alcedo::EditorSessionResult {
  if (!session_backend_) {
    alcedo::EditorSessionResult result;
    result.kind    = alcedo::EditorSessionResultKind::Rejected;
    result.state   = session_state();
    result.message = "Editor session backend is unavailable";
    return result;
  }
  return session_backend_->EditNodeGraph(change);
}

void EditorSessionController::MoveHeadToCommit(const QString& commitId) {
  const QString action = QStringLiteral("moveHeadToCommit");
  if (!session_backend_) {
    PublishHistoryRejected(action, QStringLiteral("Editor session backend is unavailable"),
                           commitId);
    return;
  }
  const QString trimmed = commitId.trimmed();
  if (trimmed.isEmpty()) {
    PublishHistoryRejected(action, QStringLiteral("Commit id is required"));
    return;
  }
  try {
    const auto id     = alcedo::Hash128::FromString(trimmed.toStdString());
    auto       result = session_backend_->MoveHeadToCommit(id);
    SyncIdentityFromBackend();
    emit StateChanged();
    PublishHistoryInvokableReturn(action, result, trimmed);
  } catch (const std::exception& ex) {
    const QString message = QString::fromUtf8(ex.what()).trimmed();
    PublishHistoryRejected(
        action, message.isEmpty() ? QStringLiteral("Invalid commit id") : message, trimmed);
  }
}

void EditorSessionController::ApplyPublishedHistory(
    const EditorHistoryOperationPublisher::Published& published) {
  Q_UNUSED(published);
  // lastHistory* getters read history_ops_.last_published(); notify QML once.
  emit HistoryOperationFinished();
}

void EditorSessionController::PublishHistoryRejected(const QString& action, const QString& message,
                                                     const QString& selected_id) {
  const auto operation_id = history_ops_.AllocateOperationId();
  ApplyPublishedHistory(history_ops_.PublishRejected(operation_id, action, message, selected_id));
}

void EditorSessionController::PublishHistoryInvokableReturn(
    const QString& action, const alcedo::EditorSessionResult& result, const QString& selected_id) {
  const auto operation_id = history_ops_.AllocateOperationId();
  ApplyPublishedHistory(
      history_ops_.PublishInvokableReturn(operation_id, action, result, selected_id));
}

void EditorSessionController::OnBackendSessionResult(const alcedo::EditorSessionResult& result) {
  auto published = history_ops_.CorrelateObservedResult(result);
  if (!published.has_value()) {
    return;
  }
  SyncIdentityFromBackend();
  ApplyPublishedHistory(*published);
}

void EditorSessionController::Close() {
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(0, 0);
  }
  if (scope_controller_) {
    scope_controller_->Shutdown();
  }
  if (!active_ && element_id_ == 0 && image_id_ == 0 &&
      session_state_ == alcedo::EditorSessionState::NoImage) {
    return;
  }
  if (session_backend_) {
    if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
      item->suspendPresentation();
    }
    SyncAlbumHdrFlagFromSnapshot();
    session_backend_->Close(/*persist_changes=*/true);
    SyncIdentityFromBackend();
  } else {
    ApplyCloseLocal();
  }
  active_ = false;
  emit StateChanged();
}

void EditorSessionController::Shutdown() {
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(0, 0);
  }
  if (scope_controller_) {
    scope_controller_->Shutdown();
  }
  if (session_backend_) {
    if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
      item->suspendPresentation();
    }
    session_backend_->Shutdown();
    SyncIdentityFromBackend();
  } else {
    ApplyCloseLocal();
    session_state_ = alcedo::EditorSessionState::ShuttingDown;
  }
  active_ = false;
  emit StateChanged();
}

void EditorSessionController::Finalize(bool persistChanges) {
  // Explicit close path for application/project lifecycle and empty-editor
  // transitions. Ordinary workspace routing deliberately does not call this.
  // The navigation layer releases guards only after save and render-idle both
  // complete, so keep presentation available for the in-flight handoff.
  if (!session_backend_) {
    if (scope_controller_) {
      scope_controller_->SetImageIdentity(0, 0);
      scope_controller_->Shutdown();
    }
    Close();
    return;
  }

  if (persistChanges) {
    SyncAlbumHdrFlagFromSnapshot();
  }

  const auto result = session_backend_->Close(persistChanges);
  SyncIdentityFromBackend();

  if (result.kind != alcedo::EditorSessionResultKind::Rejected) {
    active_ = false;
  }

  // Synchronous close can drop presentation now. Async SaveStarted keeps the
  // viewport until the backend publishes NoImage.
  if (result.kind != alcedo::EditorSessionResultKind::Rejected &&
      result.kind != alcedo::EditorSessionResultKind::SaveStarted) {
    if (scope_controller_) {
      scope_controller_->SetImageIdentity(0, 0);
      scope_controller_->Shutdown();
    }
    if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
      item->suspendPresentation();
    }
  }
  emit StateChanged();
}

void EditorSessionController::clearLastEditedImage() {
  if (last_element_id_ == 0 && last_image_id_ == 0) {
    return;
  }
  last_element_id_ = 0;
  last_image_id_   = 0;
  emit LastEditedImageChanged();
}

void EditorSessionController::bindPresentationViewport(QObject* viewportItem) {
  if (presentation_viewport_ == viewportItem) {
    return;
  }
  presentation_viewport_ = viewportItem;
  if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(viewportItem)) {
    if (scope_controller_) {
      scope_controller_->SetDownstreamSink(item->frameSink());
    }
    SyncViewportIdentity();
    SyncViewportDisplayConfig();
    // Stamp a stable presentation sink identity for render intents (Phase 5A).
    // DirectFrameSink owns the short scene-graph startup wait when this binding
    // precedes QQuickRhiItem::synchronize().
    if (session_backend_) {
      session_backend_->SetPresentationSinkId(
          static_cast<alcedo::PresentationSinkId>(reinterpret_cast<std::uintptr_t>(item)));
    }
  } else if (scope_controller_) {
    scope_controller_->SetDownstreamSink(nullptr);
  }
  emit PresentationBindingChanged();
}

void EditorSessionController::SyncViewportDisplayConfig() {
  auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (!item) {
    return;
  }

  ViewerDisplayConfig config{};
  const QVariantMap   wrapper = adjustment_snapshot_.value(QStringLiteral("odt")).toMap();
  QVariantMap         odt     = wrapper.value(QStringLiteral("odt")).toMap();
  if (odt.isEmpty()) {
    odt = wrapper;
  }

  const QString space = odt.value(QStringLiteral("encoding_space")).toString();
  if (!space.isEmpty()) {
    config.encoding_space = ColorUtils::ColorSpaceFromString(space.toStdString());
  }
  const QString eotf = odt.value(QStringLiteral("encoding_eotf")).toString();
  if (!eotf.isEmpty()) {
    config.encoding_eotf = ColorUtils::EOTFFromString(eotf.toStdString());
  }
  bool        peak_ok = false;
  const float peak    = odt.value(QStringLiteral("peak_luminance")).toFloat(&peak_ok);
  if (peak_ok && std::isfinite(peak)) {
    config.peak_luminance = std::clamp(peak, 100.0f, 10000.0f);
  }
  item->setDisplayConfig(config);
}

void EditorSessionController::SyncAlbumHdrFlagFromSnapshot() {
  if (!album_catalog_ || element_id_ == 0 || image_id_ == 0) {
    return;
  }

  const QVariantMap wrapper = adjustment_snapshot_.value(QStringLiteral("odt")).toMap();
  QVariantMap       odt     = wrapper.value(QStringLiteral("odt")).toMap();
  if (odt.isEmpty()) {
    odt = wrapper;
  }
  const QString eotf_text = odt.value(QStringLiteral("encoding_eotf")).toString();
  if (eotf_text.isEmpty()) {
    return;
  }
  const auto eotf   = ColorUtils::EOTFFromString(eotf_text.toStdString());
  const bool is_hdr = eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG;
  album_catalog_->PersistImageHdrFlag(static_cast<sl_element_id_t>(element_id_),
                                      static_cast<image_id_t>(image_id_), is_hdr);
}

void EditorSessionController::updatePresentationTargetSize(int width, int height) {
  if (session_backend_) {
    session_backend_->SetPresentationSize(width, height);
  }
}

void EditorSessionController::bindInteractionController(QObject* interactionController) {
  if (interaction_controller_ == interactionController) {
    return;
  }
  if (interaction_view_change_connection_) {
    QObject::disconnect(interaction_view_change_connection_);
    interaction_view_change_connection_ = {};
  }
  interaction_controller_ = interactionController;

  auto* interaction = qobject_cast<editor_rhi::EditorInteractionController*>(interactionController);
  if (!interaction) {
    return;
  }
  // viewChangeReported follows viewStateChanged. QML has therefore already
  // updated DirectFrameSink with the matching ROI when this route reads it.
  interaction_view_change_connection_ =
      connect(interaction, &editor_rhi::EditorInteractionController::viewChangeReported, this,
              [this](int kind) { submitViewChange(kind); });
}

void EditorSessionController::unbindPresentationViewport() {
  if (!presentation_viewport_) {
    return;
  }
  if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
    item->suspendPresentation();
  }
  if (scope_controller_) {
    scope_controller_->SetDownstreamSink(nullptr);
  }
  presentation_viewport_.clear();
  if (session_backend_) {
    session_backend_->SetPresentationSinkId(0);
  }
  emit PresentationBindingChanged();
}

void EditorSessionController::SetWorkspacePresentationActive(bool active) {
  auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (!item) {
    return;
  }
  if (!active) {
    item->suspendPresentation();
    return;
  }
  item->refreshPresentationAvailability();
}

auto EditorSessionController::presentation_viewport() const -> QObject* {
  return presentation_viewport_.data();
}

auto EditorSessionController::presentation_frame_sink() const -> alcedo::IFrameSink* {
  // Production attach path: resolve the bound QML viewport to its direct sink.
  // Pipeline code must call this (not construct a parallel sink).
  auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (!item) {
    return nullptr;
  }
  if (scope_controller_) {
    scope_controller_->SetDownstreamSink(item->frameSink());
    return scope_controller_->frame_sink();
  }
  return item->frameSink();
}

auto EditorSessionController::render_busy() const -> bool {
  // Reflects coordinator diagnostics only — never a pipeline task pointer. The
  // backend flips this via NotifyChange (fired on submit and on every render
  // result), which routes back through OnBackendChanged → StateChanged so QML
  // bindings re-evaluate (D6).
  return session_backend_ && session_backend_->render_busy();
}

auto EditorSessionController::last_error() const -> QString {
  if (!session_backend_) {
    return {};
  }
  return QString::fromUtf8(session_backend_->last_error().c_str());
}

auto EditorSessionController::first_frame_time_ms() const -> double {
  return session_backend_ ? session_backend_->first_frame_time_ms() : -1.0;
}

namespace {

auto ReasonName(alcedo::EditorRenderReason reason) -> const char* {
  using R = alcedo::EditorRenderReason;
  switch (reason) {
    case R::InitialFrame:
      return "InitialFrame";
    case R::InteractiveAdjustment:
      return "InteractiveAdjustment";
    case R::SettledAdjustment:
      return "SettledAdjustment";
    case R::ZoomPan:
      return "ZoomPan";
    case R::Resize:
      return "Resize";
    case R::DetailRefresh:
      return "DetailRefresh";
    case R::UndoRedo:
      return "UndoRedo";
    case R::ImageSwitch:
      return "ImageSwitch";
    case R::Retry:
      return "Retry";
    case R::CropRotate:
      return "CropRotate";
    case R::ScopeRefresh:
      return "ScopeRefresh";
    case R::GraphTopologyChanged:
      return "GraphTopologyChanged";
    case R::SettledMaskEdit:
      return "SettledMaskEdit";
    case R::VersionDocumentChanged:
      return "VersionDocumentChanged";
  }
  return "Unknown";
}

auto FrameRoleName(alcedo::FrameRole role) -> const char* {
  switch (role) {
    case alcedo::FrameRole::InteractivePrimary:
      return "InteractivePrimary";
    case alcedo::FrameRole::QualityBase:
      return "QualityBase";
    case alcedo::FrameRole::DetailPatch:
      return "DetailPatch";
  }
  return "Unknown";
}

}  // namespace

auto EditorSessionController::render_diagnostics() const -> QVariantMap {
  QVariantMap out;
  if (!session_backend_) {
    return out;
  }
  const auto diag = session_backend_->render_diagnostics();
  out.insert(QStringLiteral("hasInflight"), diag.has_inflight);
  out.insert(QStringLiteral("pendingCount"), static_cast<qulonglong>(diag.pending_count));
  out.insert(QStringLiteral("replacedCount"), static_cast<qulonglong>(diag.replaced_count));
  out.insert(QStringLiteral("cancelledCount"), static_cast<qulonglong>(diag.cancelled_count));
  out.insert(QStringLiteral("acceptedCount"), static_cast<qulonglong>(diag.accepted_count));
  out.insert(QStringLiteral("failedCount"), static_cast<qulonglong>(diag.failed_count));
  out.insert(QStringLiteral("readyCount"), static_cast<qulonglong>(diag.ready_count));
  out.insert(QStringLiteral("imageLoadRequestId"),
             static_cast<qulonglong>(diag.image_load_request_id));
  out.insert(QStringLiteral("lastError"), QString::fromUtf8(diag.last_error.c_str()));
  out.insert(QStringLiteral("lastRejectionReason"),
             QString::fromUtf8(diag.last_rejection_reason.c_str()));
  if (diag.inflight_reason) {
    out.insert(QStringLiteral("inflightReason"),
               QString::fromUtf8(ReasonName(*diag.inflight_reason)));
  }
  if (diag.last_rejected_render_reason) {
    out.insert(QStringLiteral("lastRejectedRenderReason"),
               QString::fromUtf8(ReasonName(*diag.last_rejected_render_reason)));
  }
  if (diag.last_ready_frame_role) {
    out.insert(QStringLiteral("lastReadyFrameRole"),
               QString::fromUtf8(FrameRoleName(*diag.last_ready_frame_role)));
  }
  if (diag.last_ready_render_reason) {
    out.insert(QStringLiteral("lastReadyRenderReason"),
               QString::fromUtf8(ReasonName(*diag.last_ready_render_reason)));
  }
  out.insert(QStringLiteral("firstFrameTimeMs"), first_frame_time_ms());
  return out;
}

void EditorSessionController::submitViewChange(int kind) {
  if (!session_backend_ || !has_image()) {
    // No backend (legacy shell) or empty editor: nothing to route. The view
    // state itself already updated in the interaction controller; the viewport
    // re-samples whatever frame it last received.
    return;
  }
  // Map the UI-level ViewChangeKind to a render reason. The interaction
  // controller owns the geometry; this controller owns only the routing seam,
  // so it must not recompute the view transform or read renderer state (D2/D7).
  auto reason = alcedo::EditorRenderReason::ZoomPan;
  switch (static_cast<editor_rhi::EditorInteractionController::ViewChangeKind>(kind)) {
    case editor_rhi::EditorInteractionController::ViewChangeKind::ZoomPan:
      reason = alcedo::EditorRenderReason::ZoomPan;
      break;
    case editor_rhi::EditorInteractionController::ViewChangeKind::Resize:
      reason = alcedo::EditorRenderReason::Resize;
      break;
    case editor_rhi::EditorInteractionController::ViewChangeKind::CropRotate:
      reason = alcedo::EditorRenderReason::CropRotate;
      break;
    case editor_rhi::EditorInteractionController::ViewChangeKind::DetailRefresh:
      reason = alcedo::EditorRenderReason::DetailRefresh;
      break;
  }
  // Only DetailRefresh carries a requested region (the visible viewport ROI).
  // The service drops any region for full-frame reasons, so we read the sink
  // region only when it is meaningful. The sink region is kept current by the
  // interaction controller's applyViewStateToViewport push, which QML runs from
  // onViewStateChanged BEFORE this handler (viewChangeReported is emitted after
  // viewStateChanged), so the region matches the just-applied view (D5).
  std::optional<alcedo::ViewportRenderRegion> region{std::nullopt};
  if (reason == alcedo::EditorRenderReason::DetailRefresh) {
    if (auto* sink = presentation_frame_sink()) {
      region = sink->GetViewportRenderRegion();
    }
  }
  session_backend_->RequestViewChange(reason, std::move(region));
}

auto EditorSessionController::can_edit() const -> bool {
  if (session_backend_) {
    return actions_.can_edit();
  }
  return false;
}

auto EditorSessionController::can_discard_current_commit() const -> bool {
  if (session_backend_) {
    return actions_.can_discard_changes();
  }
  return false;
}

bool EditorSessionController::submitPatch(QString fieldKey, QString paramsJson, bool settled) {
  auto* viewport = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (!can_edit()) {
    // Pointer release must still stop the vsync consume if edit was lost
    // mid-drag (image switch / session teardown).
    if (settled && viewport) {
      viewport->endInteractivePresentLoop();
    }
    return false;
  }
  // QQuickRhiItem::synchronize only runs after the item is marked dirty. Do
  // this on the GUI thread while handling the pointer move, before the worker
  // can block waiting for a recyclable direct-present slot. Unsettled patches
  // also arm a vsync-sampled consume so a Ready frame does not wait for the
  // next pointer event or a missed requestUpdate. The worker's NotifyFrameReady
  // update remains the completion-side wakeup when the loop is not armed.
  if (viewport) {
    if (settled) {
      viewport->endInteractivePresentLoop();
    } else {
      viewport->beginInteractivePresentLoop();
    }
    viewport->prepareForAdjustmentFrame();
  }
  alcedo::EditorAdjustmentPatch patch;
  patch.field_key              = fieldKey.toStdString();
  patch.params_json            = paramsJson.toStdString();
  patch.settled                = settled;
  // Typed models already own the live value during a pointer drag. Echoing the
  // full adjustment snapshot into QML on every interactive patch forces
  // loadFromSnapshot across Tone+Look while the mouse handler is still on the
  // stack. Rapid handoff (finish slider A → drag slider B) multiplies that with
  // history capture/commit under the pipeline render lock and freezes the GUI.
  const bool previous_suppress = suppress_snapshot_publish_;
  if (!settled) {
    suppress_snapshot_publish_ = true;
  }
  const auto result =
      settled ? session_backend_->CommitAdjustment(patch) : session_backend_->Patch(patch);
  suppress_snapshot_publish_ = previous_suppress;
  return result.kind != alcedo::EditorSessionResultKind::Rejected;
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
  const double clamped = qBound(kFilmstripExpandedHeightMin, height, kFilmstripExpandedHeightMax);
  if (qFuzzyCompare(filmstrip_expanded_height_, clamped)) {
    return;
  }
  filmstrip_expanded_height_ = clamped;
  SaveFilmstripUiPrefs();
  emit FilmstripUiChanged();
}

void EditorSessionController::set_filmstrip_scroll_position(double position) {
  const double clamped = qMax(0.0, position);
  if (qFuzzyCompare(filmstrip_scroll_position_ + 1.0, clamped + 1.0)) {
    return;
  }
  filmstrip_scroll_position_ = clamped;
  emit FilmstripUiChanged();
}

void EditorSessionController::LoadFilmstripUiPrefs() {
  QSettings settings;
  filmstrip_collapsed_ = settings.value(QLatin1String(kFilmstripCollapsedKey), false).toBool();
  filmstrip_expanded_height_ = qBound(
      kFilmstripExpandedHeightMin,
      settings.value(QLatin1String(kFilmstripExpandedHeightKey), kFilmstripExpandedHeightDefault)
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
  if (key == QLatin1String("lut") || key == QLatin1String("lmt") ||
      key == QLatin1String("ocio_lmt")) {
    return QStringLiteral("lut");
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

auto EditorSessionController::NormalizeToolPanelPage(const QString& page) -> QString {
  const QString key = page.trimmed().toLower();
  if (key == QLatin1String("history")) {
    return QStringLiteral("history");
  }
  if (key == QLatin1String("versions")) {
    return QStringLiteral("versions");
  }
  if (key == QLatin1String("nodes")) {
    return QStringLiteral("nodes");
  }
  return {};
}

auto EditorSessionController::session_generation() const -> qulonglong { return SessionEpoch(); }

auto EditorSessionController::history_revision() const -> qulonglong {
  return session_backend_ ? session_backend_->history_revision() : 0;
}

auto EditorSessionController::active_version_id() const -> QString {
  return session_backend_
             ? QString::fromStdString(session_backend_->active_version_id().ToString())
             : QString{};
}

auto EditorSessionController::pipeline_document() const -> const alcedo::PipelineDocument* {
  return session_backend_ ? session_backend_->pipeline_document() : nullptr;
}

void EditorSessionController::set_active_adjustment_panel(const QString& panel) {
  const QString normalized = NormalizeAdjustmentPanel(panel);
  if (active_adjustment_panel_ == normalized) {
    return;
  }
  active_adjustment_panel_ = normalized;
  SaveDesktopUiPrefs();
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(normalized == QLatin1String("geometry"));
    if (session_backend_->has_image() &&
        session_backend_->state() == alcedo::EditorSessionState::Interactive) {
      session_backend_->RequestViewChange(alcedo::EditorRenderReason::CropRotate, std::nullopt);
    }
  }
  emit DesktopUiChanged();
}

void EditorSessionController::set_editor_tool_panel_page(const QString& page) {
  const QString normalized = NormalizeToolPanelPage(page);
  if (editor_tool_panel_page_ == normalized) {
    return;
  }
  editor_tool_panel_page_ = normalized;
  emit DesktopUiChanged();
}

void EditorSessionController::LoadDesktopUiPrefs() {
  QSettings settings;
  active_adjustment_panel_ = NormalizeAdjustmentPanel(
      settings.value(QLatin1String(kActiveAdjustmentPanelKey), QStringLiteral("tone")).toString());
  // editorToolPanelPage is intentionally not restored from disk: collapsed on
  // cold start, but kept in memory across library/editor workspace switches.
}

void EditorSessionController::SaveDesktopUiPrefs() const {
  QSettings settings;
  settings.setValue(QLatin1String(kActiveAdjustmentPanelKey), active_adjustment_panel_);
  settings.sync();
}

auto EditorSessionController::adjustment_snapshot() const -> QVariantMap {
  return adjustment_snapshot_;
}

auto EditorSessionController::history_snapshot() const -> alcedo::EditorHistorySnapshot {
  return session_backend_ ? session_backend_->history_snapshot() : alcedo::EditorHistorySnapshot{};
}

auto EditorSessionController::PasteAdjustmentPackage(
    const alcedo::AdjustmentTransferPackage& package, const QString& versionDisplayName)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) return {};
  return session_backend_->PasteAdjustments(package, versionDisplayName.toStdString());
}

auto EditorSessionController::BuildSnapshotMap(
    const alcedo::EditorRenderAdjustmentSnapshot& snapshot) -> QVariantMap {
  QVariantMap map;
  for (const auto& patch : snapshot.patches) {
    QJsonParseError error;
    const auto      json_bytes = QByteArray::fromStdString(patch.params_json);
    auto            doc        = QJsonDocument::fromJson(json_bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
      continue;
    }
    const auto obj = doc.object();
    if (obj.isEmpty()) {
      continue;
    }
    map.insert(QString::fromStdString(patch.field_key), obj.toVariantMap());
  }
  return map;
}

}  // namespace alcedo::ui
