//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

#include <QSettings>
#include <QThread>
#include <QtGlobal>
#include <cstdint>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "edit/frame_presentation_types.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"

namespace alcedo::ui {
namespace {

constexpr auto   kFilmstripCollapsedKey          = "editor/filmstripCollapsed";
constexpr auto   kFilmstripExpandedHeightKey     = "editor/filmstripExpandedHeight";
constexpr auto   kActiveAdjustmentPanelKey       = "editor/activeAdjustmentPanel";
constexpr double kFilmstripExpandedHeightMin     = 72.0;
constexpr double kFilmstripExpandedHeightMax     = 320.0;
constexpr double kFilmstripExpandedHeightDefault = 128.0;

}  // namespace

EditorSessionController::EditorSessionController(EditorController* editor, QObject* parent)
    : EditorSessionController(editor, nullptr, parent) {}

EditorSessionController::EditorSessionController(EditorController*              editor,
                                                 alcedo::IEditorSessionBackend* session_backend,
                                                 QObject*                       parent)
    : QObject(parent), editor_(editor), session_backend_(session_backend) {
  LoadFilmstripUiPrefs();
  LoadDesktopUiPrefs();
  InstallBackendNotifier();
}

EditorSessionController::~EditorSessionController() {
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
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
}

void EditorSessionController::SetSessionBackend(alcedo::IEditorSessionBackend* session_backend) {
  if (session_backend_ == session_backend) {
    return;
  }
  if (session_backend_) {
    session_backend_->SetChangeNotifier({});
  }
  session_backend_ = session_backend;
  if (session_backend_) {
    InstallBackendNotifier();
    SyncIdentityFromBackend();
  }
}

void EditorSessionController::OnBackendChanged() {
  if (!session_backend_) {
    return;
  }
  SyncIdentityFromBackend();
  SyncViewportIdentity();
  emit StateChanged();
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
      if (auto* item =
              qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
        const bool same_image =
            session_backend_->has_image() && session_backend_->identity().element_id == elementId &&
            session_backend_->identity().image_id == imageId;
        item->setImageIdentity(static_cast<qulonglong>(imageId));
        item->setImageGeneration(static_cast<qulonglong>(
            session_backend_->identity().session_generation + (same_image ? 0 : 1)));
      }
    }
    if (elementId == 0 || imageId == 0) {
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

  Q_UNUSED(editor_);
  SyncViewportIdentity();
  emit StateChanged();
}

void EditorSessionController::Close() {
  if (!active_ && element_id_ == 0 && image_id_ == 0 &&
      session_state_ == alcedo::EditorSessionState::NoImage) {
    return;
  }
  if (session_backend_) {
    if (auto* item =
            qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
      item->suspendPresentation();
    }
    session_backend_->Close(/*persist_changes=*/true);
    SyncIdentityFromBackend();
  } else {
    ApplyCloseLocal();
  }
  active_ = false;
  emit StateChanged();
}

void EditorSessionController::Shutdown() {
  if (session_backend_) {
    if (auto* item =
            qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
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
  // Finalize closes the active image through the lifecycle owner. The QML
  // viewport unbinds itself when the workspace visual tree is destroyed.
  if (session_backend_) {
    if (auto* item =
            qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data())) {
      item->suspendPresentation();
    }
    session_backend_->Close(persistChanges);
    SyncIdentityFromBackend();
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
    if (has_image()) {
      item->setImageIdentity(static_cast<qulonglong>(image_id()));
      item->setImageGeneration(session_generation());
    }
    // Stamp a stable presentation sink identity for render intents (Phase 5A).
    if (session_backend_) {
      session_backend_->SetPresentationSinkId(
          static_cast<alcedo::PresentationSinkId>(reinterpret_cast<std::uintptr_t>(item)));
    }
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
  out.insert(QStringLiteral("sessionGeneration"),
             static_cast<qulonglong>(diag.session_generation));
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

bool EditorSessionController::submitPatch(QString fieldKey, QString paramsJson, bool settled) {
  if (!can_edit()) {
    return false;
  }
  // QQuickRhiItem::synchronize only runs after the item is marked dirty. Do
  // this on the GUI thread while handling the pointer move, before the worker
  // can block waiting for a recyclable direct-present slot. The worker's
  // NotifyFrameReady update remains the completion-side wakeup.
  auto* viewport =
      qobject_cast<editor_rhi::EditorViewportItem*>(presentation_viewport_.data());
  if (viewport) {
    viewport->prepareForAdjustmentFrame();
  }
  alcedo::EditorAdjustmentPatch patch;
  patch.field_key   = fieldKey.toStdString();
  patch.params_json = paramsJson.toStdString();
  patch.settled     = settled;
  const auto result = settled ? session_backend_->CommitAdjustment(patch)
                              : session_backend_->Patch(patch);
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
      settings.value(QLatin1String(kActiveAdjustmentPanelKey), QStringLiteral("tone")).toString());
  // historyPanelPage is intentionally not restored from disk: collapsed on
  // cold start, but kept in memory across library/editor workspace switches.
}

void EditorSessionController::SaveDesktopUiPrefs() const {
  QSettings settings;
  settings.setValue(QLatin1String(kActiveAdjustmentPanelKey), active_adjustment_panel_);
  settings.sync();
}

}  // namespace alcedo::ui
