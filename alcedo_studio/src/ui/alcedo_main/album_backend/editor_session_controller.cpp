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
#include <cstdint>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "edit/frame_presentation_types.hpp"
#include "type/hash_type.hpp"
#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::ui {
namespace {

constexpr auto   kFilmstripCollapsedKey          = "editor/filmstripCollapsed";
constexpr auto   kFilmstripExpandedHeightKey     = "editor/filmstripExpandedHeight";
constexpr auto   kActiveAdjustmentPanelKey       = "editor/activeAdjustmentPanel";
constexpr double kFilmstripExpandedHeightMin     = 72.0;
constexpr double kFilmstripExpandedHeightMax     = 320.0;
constexpr double kFilmstripExpandedHeightDefault = 128.0;

auto             EmptyRawDecodeCapabilities() -> QVariantMap {
  return {
      {QStringLiteral("rawSource"), false},
      {QStringLiteral("available"), false},
      {QStringLiteral("metadataAvailable"), false},
      {QStringLiteral("neuralEngineAvailable"), false},
      {QStringLiteral("highlightsAvailable"), false},
      {QStringLiteral("unavailableReason"),
                   QStringLiteral("Select a RAW image to enable RAW Decode.")},
      {QStringLiteral("methodValues"), QVariantList{}},
      {QStringLiteral("rawDefaultParamsJson"), QString{}},
  };
}

}  // namespace

EditorSessionController::EditorSessionController(EditorController* editor, QObject* parent)
    : EditorSessionController(editor, nullptr, parent) {}

EditorSessionController::EditorSessionController(EditorController*              editor,
                                                 alcedo::IEditorSessionBackend* session_backend,
                                                 QObject*                       parent)
    : QObject(parent), editor_(editor), session_backend_(session_backend) {
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
  scope_controller_->SetImageIdentity(image_id(), session_generation());
  LoadFilmstripUiPrefs();
  LoadDesktopUiPrefs();
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(active_adjustment_panel_ ==
                                               QLatin1String("geometry"));
    SyncIdentityFromBackend();
  }
  InstallBackendNotifier();
  SyncRawDecodeCapabilities();
}

EditorSessionController::~EditorSessionController() {
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
    session_backend_->SetResultObserver({});
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
}

void EditorSessionController::SetSessionBackend(alcedo::IEditorSessionBackend* session_backend) {
  if (session_backend_ == session_backend) {
    return;
  }
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
    session_backend_->SetResultObserver({});
  }
  session_backend_ = session_backend;
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(active_adjustment_panel_ ==
                                               QLatin1String("geometry"));
    InstallBackendNotifier();
    SyncIdentityFromBackend();
  }
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(image_id(), session_generation());
  }
  SyncRawDecodeCapabilities();
}

void EditorSessionController::OnBackendChanged() {
  if (!session_backend_) {
    return;
  }
  SyncIdentityFromBackend();
  SyncRawDecodeCapabilities();
  SyncViewportIdentity();

  // Phase 6C-7: keep the cached snapshot map warm on every backend change, but
  // only emit AdjustmentSnapshotChanged when not suppressed. Interactive
  // submitPatch suppresses the emit so each pointer move does not re-enter
  // EditorAdjustmentStack.loadFromSnapshot (QML signal storm → GUI stall when
  // switching sliders rapidly while history/render also touch the pipeline).
  const auto render_snapshot = session_backend_->adjustment_snapshot();
  auto       panel_snapshot  = BuildSnapshotMap(render_snapshot);
  if (panel_snapshot != adjustment_snapshot_) {
    adjustment_snapshot_ = std::move(panel_snapshot);
    ++snapshot_revision_;
    if (!suppress_snapshot_publish_) {
      emit AdjustmentSnapshotChanged();
    }
  }
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
    return session_backend_->has_image();
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

auto EditorSessionController::session_generation() const -> qulonglong {
  if (session_backend_) {
    return session_backend_->identity().session_generation;
  }
  return session_generation_;
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
  const auto id       = session_backend_->identity();
  element_id_         = id.element_id;
  image_id_           = id.image_id;
  session_generation_ = id.session_generation;
  session_state_      = session_backend_->state();
  // active_ is workspace membership owned by Open/Close/Finalize, not by
  // backend NoImage vs Loading (empty editor remains active).
}

void EditorSessionController::SyncRawDecodeCapabilities() {
  const QVariantMap next =
      editor_ ? editor_->RawDecodeCapabilitiesForImage(image_id()) : EmptyRawDecodeCapabilities();
  if (next == raw_decode_capabilities_) {
    return;
  }
  raw_decode_capabilities_ = next;
  emit RawDecodeCapabilitiesChanged();
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
  if (scope_controller_) {
    scope_controller_->SetImageIdentity(image_id(), session_generation());
  }
  if (auto* item = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
    if (has_image()) {
      item->setImageIdentity(static_cast<qulonglong>(image_id()));
      item->setImageGeneration(session_generation());
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
      if (scope_controller_) {
        scope_controller_->SetImageIdentity(
            static_cast<qulonglong>(imageId),
            static_cast<qulonglong>(session_backend_->identity().session_generation +
                                    (same_image ? 0 : 1)));
      }
      if (auto* item =
              qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
        item->setImageIdentity(static_cast<qulonglong>(imageId));
        item->setImageGeneration(static_cast<qulonglong>(
            session_backend_->identity().session_generation + (same_image ? 0 : 1)));
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

  SyncRawDecodeCapabilities();
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
    session_backend_->Close(/*persist_changes=*/true);
    SyncIdentityFromBackend();
  } else {
    ApplyCloseLocal();
  }
  SyncRawDecodeCapabilities();
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
  SyncRawDecodeCapabilities();
  active_ = false;
  emit StateChanged();
}

void EditorSessionController::Finalize(bool persistChanges) {
  // Finalize closes the active image through the lifecycle owner. The QML
  // viewport unbinds itself when the workspace visual tree is destroyed.
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
    session_backend_->Close(persistChanges);
    SyncIdentityFromBackend();
    SyncRawDecodeCapabilities();
    active_ = false;
    emit StateChanged();
    return;
  }
  Close();
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
    if (has_image()) {
      item->setImageIdentity(static_cast<qulonglong>(image_id()));
      item->setImageGeneration(session_generation());
    }
    // Stamp a stable presentation sink identity for render intents (Phase 5A).
    if (session_backend_) {
      session_backend_->SetPresentationSinkId(
          static_cast<alcedo::PresentationSinkId>(reinterpret_cast<std::uintptr_t>(item)));
    }
  } else if (scope_controller_) {
    scope_controller_->SetDownstreamSink(nullptr);
  }
  emit PresentationBindingChanged();
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
  out.insert(QStringLiteral("presentedCount"), static_cast<qulonglong>(diag.presented_count));
  out.insert(QStringLiteral("sessionGeneration"), static_cast<qulonglong>(diag.session_generation));
  out.insert(QStringLiteral("renderGeneration"), static_cast<qulonglong>(diag.render_generation));
  out.insert(QStringLiteral("viewGeneration"), static_cast<qulonglong>(diag.view_generation));
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
  if (diag.last_submitted_frame_role) {
    out.insert(QStringLiteral("lastSubmittedFrameRole"),
               QString::fromUtf8(FrameRoleName(*diag.last_submitted_frame_role)));
  }
  if (diag.last_submitted_render_reason) {
    out.insert(QStringLiteral("lastSubmittedRenderReason"),
               QString::fromUtf8(ReasonName(*diag.last_submitted_render_reason)));
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
  if (!session_backend_) {
    return false;
  }
  return session_backend_->has_image() &&
         session_backend_->state() == alcedo::EditorSessionState::Interactive;
}

auto EditorSessionController::can_discard_current_commit() const -> bool {
  if (!session_backend_ || !has_image() ||
      session_backend_->state() != alcedo::EditorSessionState::Interactive) {
    return false;
  }
  return session_backend_->has_unmaterialized_changes();
}

bool EditorSessionController::submitPatch(QString fieldKey, QString paramsJson, bool settled) {
  if (!can_edit()) {
    return false;
  }
  // QQuickRhiItem::synchronize only runs after the item is marked dirty. Do
  // this on the GUI thread while handling the pointer move, before the worker
  // can block waiting for a recyclable direct-present slot. The worker's
  // NotifyFrameReady update remains the completion-side wakeup.
  auto* viewport = qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (viewport) {
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
  if (session_backend_) {
    session_backend_->SetGeometryOverlayActive(normalized == QLatin1String("geometry"));
    if (session_backend_->has_image() &&
        session_backend_->state() == alcedo::EditorSessionState::Interactive) {
      session_backend_->RequestViewChange(alcedo::EditorRenderReason::CropRotate, std::nullopt);
    }
  }
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
      settings.value(QLatin1String(kActiveAdjustmentPanelKey), QStringLiteral("tone")).toString());
  // historyPanelPage is intentionally not restored from disk: collapsed on
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

auto EditorSessionController::BeginMergeAdjustmentPackage(
    const alcedo::AdjustmentTransferPackage& package, alcedo::AdjustmentMergePreview* preview)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) return {};
  return session_backend_->BeginMerge(package, preview);
}

auto EditorSessionController::CompleteMergeAdjustments(
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions)
    -> alcedo::EditorSessionResult {
  if (!session_backend_) return {};
  return session_backend_->CompleteMerge(resolutions);
}

auto EditorSessionController::CancelMergeAdjustments() -> alcedo::EditorSessionResult {
  if (!session_backend_) return {};
  return session_backend_->CancelMerge();
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
