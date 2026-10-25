//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_ai_mask_tool.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QLineF>
#include <QMetaObject>
#include <QVariantMap>
#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include "sidecar_client/segmentation_client.hpp"
#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/edit_viewer/ai_mask_prompt_geometry.hpp"

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;

/// A press that moves less than this many item pixels before release is a click prompt.
constexpr qreal    kBoxDragThresholdPx    = 6.0;
/// A cold sidecar start loads no CLIP model here, but the process start and the gRPC readiness
/// poll still need more than the default 5 s on a cold disk (same value as image analysis).
constexpr auto     kSidecarStartupTimeout = 60s;

[[nodiscard]] auto ToQString(const std::string& value) -> QString {
  return QString::fromStdString(value);
}

[[nodiscard]] auto PathText(const std::filesystem::path& path) -> QString {
  return QString::fromStdU16String(path.u16string());
}

[[nodiscard]] auto TaskKindFromName(const QString& name) -> std::optional<AiMaskTaskKind> {
  if (name == QLatin1String("subject")) {
    return AiMaskTaskKind::Subject;
  }
  if (name == QLatin1String("background")) {
    return AiMaskTaskKind::Background;
  }
  if (name == QLatin1String("sky")) {
    return AiMaskTaskKind::Sky;
  }
  if (name == QLatin1String("people")) {
    return AiMaskTaskKind::People;
  }
  return std::nullopt;
}

[[nodiscard]] auto ElapsedMs(std::chrono::steady_clock::time_point start) -> qint64 {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

}  // namespace

EditorAiMaskTool::EditorAiMaskTool(EditorSessionController* session, Services services,
                                   QObject* parent)
    : QObject(parent), session_(session), services_(std::move(services)) {
  worker_thread_.setObjectName(QStringLiteral("AiMaskToolWorker"));
  worker_context_ = new QObject();
  worker_context_->moveToThread(&worker_thread_);
  worker_thread_.start();
  if (session_ != nullptr) {
    result_connection_ =
        connect(session_, &EditorSessionController::SessionResultObserved, this,
                [this](const EditorSessionResult& result) { OnSessionResult(result); });
    state_connection_ = connect(session_, &EditorSessionController::StateChanged, this,
                                [this] { OnSessionStateChanged(); });
  }
}

EditorAiMaskTool::~EditorAiMaskTool() {
  Shutdown();
  delete worker_context_;
}

void EditorAiMaskTool::Shutdown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  QObject::disconnect(result_connection_);
  QObject::disconnect(state_connection_);
  if (active_) {
    Close({});
  }
  // The worker can wait in a blocking queued call to the GUI thread (sidecar start, lease), so
  // the GUI thread keeps processing events until the worker drained its queue and stopped.
  QMetaObject::invokeMethod(
      worker_context_, [] { QThread::currentThread()->quit(); }, Qt::QueuedConnection);
  while (!worker_thread_.wait(25)) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  }
}

// ── Open ───────────────────────────────────────────────────────────────────

void EditorAiMaskTool::open() { OpenFor(std::nullopt); }

void EditorAiMaskTool::openSelectedMask() {
  auto* adapter = session_ != nullptr ? session_->mask_creation() : nullptr;
  if (adapter == nullptr || adapter->selected_mask_id().isEmpty()) {
    ShowError(QStringLiteral("Select an AI Mask in the Mask list first."));
    return;
  }
  OpenFor(MaskId{adapter->selected_mask_id().toStdString()});
}

void EditorAiMaskTool::ShowError(QString error) {
  if (active_ || shut_down_) {
    return;
  }
  ++serial_;
  active_ = true;
  ready_  = false;
  Fail(std::move(error));
}

void EditorAiMaskTool::OpenFor(std::optional<MaskId> mask_id) {
  if (active_ || shut_down_) {
    return;
  }
  ++serial_;
  active_              = true;
  ready_               = false;
  lease_open_          = false;
  commit_operation_id_ = 0;
  busy_text_.clear();
  error_text_.clear();
  image_text_.clear();
  last_step_text_.clear();
  view_     = {};
  preview_  = {};
  pointer_  = {};
  mask_id_  = std::move(mask_id);
  grade_id_ = {};
  emit toolChanged();
  emit previewChanged();
  if (Backend() == nullptr || session_ == nullptr || !session_->has_image()) {
    Fail(QStringLiteral("Open an image in the editor first."));
    return;
  }
  if (mask_id_.has_value()) {
    grade_id_ = session_->mask_creation()->edit_node_id();
  } else {
    grade_id_ = SelectedGradeId();
  }
  if (grade_id_.Empty()) {
    Fail(QStringLiteral("Select a Color Grade node first."));
    return;
  }
  StartRequest request;
  request.serial  = serial_;
  request.runtime = services_.runtime ? services_.runtime() : nullptr;
  if (!request.runtime) {
    Fail(QStringLiteral("The AI sidecar runtime is not available. Open a project first."));
    return;
  }
  if (services_.runtime_options) {
    request.runtime_options = services_.runtime_options();
  }
  if (services_.model_directory) {
    request.model_directory = services_.model_directory();
  }

  SetBusy(QStringLiteral("Opening the AI Mask edit..."));
  const auto                 serial = serial_;
  QPointer<EditorAiMaskTool> self(this);
  auto                       shared_request = std::make_shared<StartRequest>(std::move(request));
  const auto                 submitted      = Backend()->OpenAiMaskEdit(
      grade_id_, mask_id_, [self, serial, shared_request](EditorAiMaskEditOpenResult opened) {
        QMetaObject::invokeMethod(
            self,
            [self, serial, shared_request, opened = std::move(opened)]() mutable {
              if (!self) {
                return;
              }
              if (opened.result.kind == EditorSessionResultKind::Accepted) {
                shared_request->geometry_fingerprint = opened.geometry_fingerprint;
                shared_request->stored_source = std::move(opened.stored_source);
                shared_request->stored_asset  = std::move(opened.stored_asset);
              }
              self->OnOpened(serial, std::move(opened));
              if (self && self->lease_open_ && serial == self->serial_) {
                // The analysis image of the open image: whole source, current Develop (SM4).
                auto* backend = self->Backend();
                self->SetBusy(QStringLiteral("Rendering the analysis image..."));
                const auto rendered = backend->RenderAiMaskAnalysisImage(
                    EditorAiMaskAnalysisContent::KeyAndPixels,
                    [self, serial, shared_request](EditorAiMaskAnalysisResult analysis) {
                      QMetaObject::invokeMethod(
                          self,
                          [self, serial, shared_request, analysis = std::move(analysis)]() mutable {
                            if (self) {
                              self->OnAnalysisRendered(serial, std::move(analysis),
                                                                            std::move(*shared_request));
                            }
                          },
                          Qt::QueuedConnection);
                    });
                if (rendered.kind == EditorSessionResultKind::Rejected) {
                  self->Fail(ToQString(rendered.message));
                }
              }
            },
            Qt::QueuedConnection);
      });
  if (submitted.kind == EditorSessionResultKind::Rejected) {
    Fail(ToQString(submitted.message));
  }
}

void EditorAiMaskTool::OnOpened(std::uint64_t serial, EditorAiMaskEditOpenResult opened) {
  if (serial != serial_ || !active_) {
    return;
  }
  if (opened.result.kind != EditorSessionResultKind::Accepted) {
    Fail(QStringLiteral("The AI Mask edit did not open: %1").arg(ToQString(opened.result.message)));
    return;
  }
  lease_open_ = true;
  emit toolChanged();
}

void EditorAiMaskTool::OnAnalysisRendered(std::uint64_t serial, EditorAiMaskAnalysisResult analysis,
                                          StartRequest request) {
  if (serial != serial_ || !active_) {
    return;
  }
  if (analysis.status != EditorAiMaskAnalysisStatus::Completed) {
    Fail(QStringLiteral("The AI Mask analysis image failed: %1").arg(ToQString(analysis.message)));
    return;
  }
  SetBusy(QStringLiteral("Starting the AI sidecar and loading the AI Mask models..."));
  auto shared_analysis = std::make_shared<EditorAiMaskAnalysisResult>(std::move(analysis));
  auto shared_request  = std::make_shared<StartRequest>(std::move(request));
  PostToWorker([this, shared_analysis, shared_request](Worker& /*worker*/) {
    StartOnWorker(std::move(*shared_analysis), std::move(*shared_request));
  });
}

void EditorAiMaskTool::StartOnWorker(EditorAiMaskAnalysisResult analysis, StartRequest request) {
  const auto serial = request.serial;
  const auto fail   = [this, serial](QString message) {
    qWarning().noquote() << "[AI Mask]" << message;
    QMetaObject::invokeMethod(
        this,
        [this, serial, message] {
          if (serial == serial_ && active_) {
            Fail(message);
          }
        },
        Qt::QueuedConnection);
  };
  // A Cancel queues the close of the earlier session before this start.
  worker_.session.reset();
  worker_.service.reset();

  const auto start   = std::chrono::steady_clock::now();
  auto&      runtime = request.runtime;
  // The lease keeps the process running from now until the service closes.
  auto       lease   = runtime->AcquireLease();
  if (!lease) {
    fail(QStringLiteral("The AI sidecar lease is not available."));
    return;
  }
  if (runtime->Status().state != AiSidecarRuntimeState::kReady) {
    auto options               = request.runtime_options;
    options.allow_download     = false;
    options.require_model_info = false;
    options.startup_timeout    = kSidecarStartupTimeout;
    if (!runtime->StartAndWaitInteractive(options)) {
      fail(QStringLiteral("The AI sidecar did not start: %1")
               .arg(ToQString(runtime->Status().message)));
      return;
    }
  }
  const auto sidecar_ms = ElapsedMs(start);
  const auto endpoint   = runtime->Status().endpoint;
  auto       service    = std::make_unique<AiMaskSegmentationService>(
      std::move(lease), sidecar_client::MakeGrpcSegmentationClient(endpoint));

  const auto load_start = std::chrono::steady_clock::now();
  const auto loaded     = service->LoadModels(request.model_directory);
  if (!loaded.ok) {
    fail(QStringLiteral("The AI Mask models could not be loaded from %1: %2")
             .arg(PathText(request.model_directory), ToQString(loaded.error)));
    return;
  }
  const auto load_ms = ElapsedMs(load_start);
  const auto models  = service->Status();
  if (!models.status.ok) {
    fail(QStringLiteral("The AI Mask model status could not be read: %1")
             .arg(ToQString(models.status.error)));
    return;
  }

  const auto image_start = std::chrono::steady_clock::now();
  const auto started     = service->StartImage(analysis.image, analysis.image_key);
  if (!started.status.ok) {
    fail(QStringLiteral("The segmentation session did not start: %1")
             .arg(ToQString(started.status.error)));
    return;
  }
  const auto         image_ms = ElapsedMs(image_start);

  AiMaskSessionImage image;
  image.extent               = Extent2D{started.image_width, started.image_height};
  image.model_set_id         = models.model_set_id;
  image.geometry_fingerprint = request.geometry_fingerprint;
  try {
    if (request.stored_source.has_value() && request.stored_asset.has_value()) {
      worker_.session = std::make_unique<AiMaskEditSession>(*service, image, *request.stored_source,
                                                            std::move(*request.stored_asset));
    } else {
      worker_.session = std::make_unique<AiMaskEditSession>(*service, image);
    }
  } catch (const std::exception& error) {
    fail(QStringLiteral("The AI Mask session did not start: %1")
             .arg(QString::fromUtf8(error.what())));
    return;
  }
  worker_.service = std::move(service);

  const auto image_text =
      QStringLiteral(
          "%1 x %2 | %3 on %4 (%5) | sidecar %6 ms, load %7 ms, image %8 ms "
          "(encoder %9 ms%10)")
          .arg(started.image_width)
          .arg(started.image_height)
          .arg(ToQString(models.model_set_id), ToQString(models.backend), ToQString(models.device))
          .arg(sidecar_ms)
          .arg(load_ms)
          .arg(image_ms)
          .arg(started.encoder_ms)
          .arg(started.from_cache ? QStringLiteral(", cached") : QString{});
  qInfo().noquote() << "[AI Mask] session started:" << image_text;
  auto view = ReadView(*worker_.session);
  QMetaObject::invokeMethod(
      this,
      [this, serial, image_text, view = std::move(view)]() mutable {
        OnStarted(serial, image_text, std::move(view));
      },
      Qt::QueuedConnection);
}

void EditorAiMaskTool::OnStarted(std::uint64_t serial, QString image_text, SessionView view) {
  if (serial != serial_ || !active_) {
    return;
  }
  ready_      = true;
  image_text_ = std::move(image_text);
  view_       = std::move(view);
  busy_text_.clear();
  emit toolChanged();
  // A stored Mask shows its saved coverage without a model call (M15); a new Mask shows the
  // empty candidate, which also replaces a committed coverage preview.
  PresentCandidate(view_.candidate);
}

// ── Session steps ──────────────────────────────────────────────────────────

void EditorAiMaskTool::setSubtract(bool subtract) {
  if (subtract_ == subtract) {
    return;
  }
  subtract_ = subtract;
  emit toolChanged();
}

void EditorAiMaskTool::setInputMode(const QString& mode) {
  const auto target =
      mode == QLatin1String("task") ? AiMaskInputMode::Task : AiMaskInputMode::Prompt;
  if (target == view_.input_mode) {
    return;
  }
  RunStep(QStringLiteral("Changing the input mode..."),
          [target](AiMaskEditSession& session) { return session.SetInputMode(target); });
}

void EditorAiMaskTool::addTask(const QString& task) {
  const auto kind = TaskKindFromName(task);
  if (!kind.has_value()) {
    Fail(QStringLiteral("Unknown AI Mask task: %1").arg(task));
    return;
  }
  const auto operation =
      subtract_ ? AiMaskComponentOperation::Subtract : AiMaskComponentOperation::Add;
  RunStep(QStringLiteral("Running the %1 task...").arg(task),
          [kind = *kind, operation](AiMaskEditSession& session) {
            return session.AddTask(kind, operation);
          });
}

void EditorAiMaskTool::togglePerson(int person_index) {
  if (person_index < 0) {
    return;
  }
  RunStep(QStringLiteral("Running the person mask..."),
          [index = static_cast<std::uint32_t>(person_index)](AiMaskEditSession& session) {
            return session.TogglePerson(index);
          });
}

void EditorAiMaskTool::selectLevel(int level) {
  if (level < 0) {
    return;
  }
  RunStep(QStringLiteral("Selecting the level..."),
          [level = static_cast<std::uint32_t>(level)](AiMaskEditSession& session) {
            return session.SelectLevel(level);
          });
}

void EditorAiMaskTool::previewEdge(qreal feather_px, int expand_px) {
  if (!ready_ || !view_.candidate.pixels) {
    return;
  }
  auto candidate = view_.candidate;
  candidate.edge = AiMaskEdgeAdjustment{
      static_cast<float>(std::clamp<qreal>(feather_px, 0.0, kAiMaskMaximumFeatherPx)),
      std::clamp(expand_px, -kAiMaskMaximumExpandPx, kAiMaskMaximumExpandPx)};
  PresentCandidate(candidate);
}

void EditorAiMaskTool::acceptEdge(qreal feather_px, int expand_px) {
  const AiMaskEdgeAdjustment edge{
      static_cast<float>(std::clamp<qreal>(feather_px, 0.0, kAiMaskMaximumFeatherPx)),
      std::clamp(expand_px, -kAiMaskMaximumExpandPx, kAiMaskMaximumExpandPx)};
  if (edge == view_.edge) {
    // The drag returned to the accepted value; show the accepted coverage again.
    PresentCandidate(view_.candidate);
    return;
  }
  RunStep(QStringLiteral("Applying Feather and Expand..."),
          [edge](AiMaskEditSession& session) { return session.SetEdgeAdjustment(edge); });
}

void EditorAiMaskTool::undo() {
  RunStep(QStringLiteral("Undo..."), [](AiMaskEditSession& session) {
    return session.Undo() ? AiMaskStepResult{}
                          : AiMaskStepResult{AiMaskStepStatus::Rejected, "Nothing to undo"};
  });
}

void EditorAiMaskTool::redo() {
  RunStep(QStringLiteral("Redo..."), [](AiMaskEditSession& session) {
    return session.Redo() ? AiMaskStepResult{}
                          : AiMaskStepResult{AiMaskStepStatus::Rejected, "Nothing to redo"};
  });
}

void EditorAiMaskTool::RunStep(QString busy_text, SessionOperation operation) {
  if (!ready_ || busy()) {
    return;
  }
  error_text_.clear();
  SetBusy(std::move(busy_text));
  const auto serial = serial_;
  PostToWorker([this, serial, operation = std::move(operation)](Worker& worker) {
    AiMaskStepResult result{AiMaskStepStatus::Rejected, "The AI Mask session is closed"};
    SessionView      view;
    const auto       start = std::chrono::steady_clock::now();
    if (worker.session) {
      try {
        result = operation(*worker.session);
      } catch (const std::exception& error) {
        result = AiMaskStepResult{AiMaskStepStatus::Failed, error.what()};
      }
      view = ReadView(*worker.session);
    }
    const auto elapsed = ElapsedMs(start);
    qInfo().noquote() << "[AI Mask] step" << StepResultText(result) << "in" << elapsed << "ms";
    QMetaObject::invokeMethod(
        this,
        [this, serial, result, elapsed, view = std::move(view)]() mutable {
          if (serial != serial_ || !active_) {
            return;
          }
          last_step_text_ = QStringLiteral("%1 (%2 ms)").arg(StepResultText(result)).arg(elapsed);
          OnStepFinished(serial, std::move(result), std::move(view));
        },
        Qt::QueuedConnection);
  });
}

void EditorAiMaskTool::OnStepFinished(std::uint64_t /*serial*/, AiMaskStepResult result,
                                      SessionView view) {
  busy_text_.clear();
  const bool applied = result.status == AiMaskStepStatus::Applied;
  if (applied) {
    view_ = std::move(view);
  } else if (result.status == AiMaskStepStatus::Failed) {
    error_text_ = ToQString(result.message);
  }
  emit toolChanged();
  // A step that did not apply keeps the session coverage; a Feather or Expand preview of a slider
  // that the step did not accept returns to it.
  PresentCandidate(view_.candidate);
}

auto EditorAiMaskTool::ReadView(const AiMaskEditSession& session) -> SessionView {
  SessionView view;
  view.can_undo     = session.CanUndo();
  view.can_redo     = session.CanRedo();
  view.state_count  = static_cast<int>(session.StateCount());
  view.input_mode   = session.InputMode();
  view.edge         = session.Edge();
  const auto people = session.People();
  for (std::size_t index = 0; index < people.size(); ++index) {
    QVariantMap person;
    person.insert(QStringLiteral("index"), static_cast<int>(index));
    person.insert(QStringLiteral("score"), people[index].score);
    person.insert(QStringLiteral("included"),
                  session.PersonIncluded(static_cast<std::uint32_t>(index)));
    view.people.push_back(person);
  }
  view.candidate.pixels          = session.AcquireCoveragePixels();
  view.candidate.analysis_extent = session.Image().extent;
  view.candidate.edge            = session.Edge();
  return view;
}

auto EditorAiMaskTool::StepResultText(const AiMaskStepResult& result) -> QString {
  switch (result.status) {
    case AiMaskStepStatus::Applied:
      return QStringLiteral("Applied");
    case AiMaskStepStatus::NotFound:
      return QStringLiteral("Not found: %1").arg(ToQString(result.message));
    case AiMaskStepStatus::Failed:
      return QStringLiteral("Failed: %1").arg(ToQString(result.message));
    case AiMaskStepStatus::Rejected:
      return QStringLiteral("Rejected: %1").arg(ToQString(result.message));
  }
  return {};
}

// ── Coverage preview ───────────────────────────────────────────────────────

void EditorAiMaskTool::PresentCandidate(const MaskCoverageCandidateSelection& candidate) {
  auto* backend = Backend();
  if (backend == nullptr || !lease_open_ || !candidate.pixels) {
    return;
  }
  preview_.status = EditorMaskCoveragePreviewStatus::Pending;
  QPointer<EditorAiMaskTool> self(this);
  const auto                 submitted =
      backend->UpdateAiMaskCandidateCoverage(candidate, [self](EditorMaskCoverageResult result) {
        QMetaObject::invokeMethod(
            self,
            [self, result = std::move(result)]() mutable {
              if (self) {
                self->OnCoveragePresented(std::move(result));
              }
            },
            Qt::QueuedConnection);
      });
  if (submitted.kind == EditorSessionResultKind::Rejected) {
    preview_.status = EditorMaskCoveragePreviewStatus::Previous;
    preview_.error  = submitted.message;
  }
  emit previewChanged();
}

void EditorAiMaskTool::OnCoveragePresented(EditorMaskCoverageResult result) {
  if (!active_) {
    return;
  }
  // A superseded request leaves its successor in flight; its completion carries the state.
  if (result.outcome == EditorMaskCoverageOutcome::Superseded) {
    return;
  }
  preview_ = result.state;
  if (result.outcome == EditorMaskCoverageOutcome::Rejected && preview_.error.empty()) {
    preview_.error = result.message;
  }
  if (result.outcome == EditorMaskCoverageOutcome::Failed) {
    qWarning().noquote() << "[AI Mask] coverage presentation failed:" << ToQString(result.message);
  }
  emit previewChanged();
}

void EditorAiMaskTool::retryPreview() {
  auto* backend = Backend();
  if (backend == nullptr || !lease_open_) {
    return;
  }
  QPointer<EditorAiMaskTool> self(this);
  (void)backend->RetryMaskCoveragePreview([self](EditorMaskCoverageResult result) {
    QMetaObject::invokeMethod(
        self,
        [self, result = std::move(result)]() mutable {
          if (self) {
            self->OnCoveragePresented(std::move(result));
          }
        },
        Qt::QueuedConnection);
  });
}

// ── Commit and Cancel ──────────────────────────────────────────────────────

void EditorAiMaskTool::commit() {
  if (!ready_ || busy()) {
    return;
  }
  error_text_.clear();
  SetBusy(QStringLiteral("Committing the AI Mask..."));
  const auto serial = serial_;
  PostToWorker([this, serial](Worker& worker) {
    auto payload =
        std::make_shared<std::optional<std::pair<MaskAsset, AiSegmentationMaskSource>>>();
    QString error;
    if (!worker.session) {
      error = QStringLiteral("The AI Mask session is closed.");
    } else {
      try {
        *payload = worker.session->PrepareCommit();
      } catch (const std::exception& prepare_error) {
        error = QString::fromUtf8(prepare_error.what());
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, serial, payload, error] {
          if (serial != serial_ || !active_) {
            return;
          }
          auto* backend = Backend();
          if (!payload->has_value() || backend == nullptr) {
            Fail(error.isEmpty() ? QStringLiteral("The editor session is not available.") : error);
            return;
          }
          EditorAiMaskCommit commit;
          commit.grade_id   = grade_id_;
          commit.mask_id    = mask_id_;
          commit.asset      = std::move((*payload)->first);
          commit.source     = std::move((*payload)->second);
          const auto queued = backend->CommitAiMaskEdit(std::move(commit));
          if (queued.kind == EditorSessionResultKind::Rejected ||
              queued.kind == EditorSessionResultKind::Failed) {
            Fail(ToQString(queued.message));
            return;
          }
          // The result of the queued Commit arrives through the session result observer.
          commit_operation_id_ = queued.operation_id;
        },
        Qt::QueuedConnection);
  });
}

void EditorAiMaskTool::OnSessionResult(const EditorSessionResult& result) {
  if (commit_operation_id_ == 0 || result.operation_id != commit_operation_id_) {
    return;
  }
  if (result.kind == EditorSessionResultKind::Rejected ||
      result.kind == EditorSessionResultKind::Failed) {
    // The session stays open: the asset write or the history publication failed (SM9.3).
    commit_operation_id_ = 0;
    Fail(QStringLiteral("The AI Mask was not committed: %1").arg(ToQString(result.message)));
    return;
  }
  auto* backend = Backend();
  if (backend != nullptr && !backend->ai_mask_edit_open()) {
    commit_operation_id_ = 0;
    lease_open_          = false;
    qInfo().noquote() << "[AI Mask] committed:" << ToQString(result.message);
    Close({});
  }
}

void EditorAiMaskTool::OnSessionStateChanged() {
  if (!active_ || !lease_open_ || commit_operation_id_ != 0) {
    return;
  }
  auto* backend = Backend();
  if (backend != nullptr && !backend->ai_mask_edit_open()) {
    // The editor ended the edit (image change or close) and released the lease.
    lease_open_ = false;
    qInfo().noquote() << "[AI Mask] the editor ended the AI Mask edit";
    Close({});
  }
}

void EditorAiMaskTool::cancel() {
  if (active_) {
    Close({});
  }
}

void EditorAiMaskTool::Close(QString error) {
  // Results of this Open that are still queued carry the old serial and are ignored.
  ++serial_;
  if (auto* backend = Backend(); backend != nullptr && (lease_open_ || busy())) {
    // Queued after an Open in flight, so it also ends an edit that opens after this call.
    (void)backend->CancelAiMaskEdit();
  }
  PostToWorker([](Worker& worker) {
    worker.session.reset();
    if (worker.service) {
      const auto closed = worker.service->Close();
      if (!closed.ok) {
        qWarning().noquote() << "[AI Mask] closing the segmentation service failed:"
                             << ToQString(closed.error);
      }
      worker.service.reset();
    }
  });
  active_              = false;
  ready_               = false;
  lease_open_          = false;
  commit_operation_id_ = 0;
  busy_text_.clear();
  error_text_ = std::move(error);
  view_       = {};
  preview_    = {};
  pointer_    = {};
  mask_id_.reset();
  emit toolChanged();
  emit previewChanged();
  emit boxChanged();
}

void EditorAiMaskTool::Fail(QString error) {
  qWarning().noquote() << "[AI Mask]" << error;
  busy_text_.clear();
  error_text_ = std::move(error);
  emit toolChanged();
}

void EditorAiMaskTool::SetBusy(QString text) {
  busy_text_ = std::move(text);
  emit toolChanged();
}

void EditorAiMaskTool::PostToWorker(std::function<void(Worker& worker)> task) {
  QMetaObject::invokeMethod(
      worker_context_, [this, task = std::move(task)] { task(worker_); }, Qt::QueuedConnection);
}

// ── Viewport input ─────────────────────────────────────────────────────────

void EditorAiMaskTool::bindInteractionItem(QObject* interaction) {
  interaction_ = qobject_cast<editor_rhi::EditorInteractionController*>(interaction);
}

auto EditorAiMaskTool::handlePress(qreal x, qreal y, int button) -> bool {
  if (!owns_left_button() || button != Qt::LeftButton) {
    return false;
  }
  pointer_ = PointerInput{true, false, QPointF(x, y), QPointF(x, y)};
  return true;
}

void EditorAiMaskTool::handleMove(qreal x, qreal y, int /*buttons*/) {
  if (!pointer_.pressed) {
    return;
  }
  pointer_.last = QPointF(x, y);
  if (!pointer_.box && QLineF(pointer_.press, pointer_.last).length() > kBoxDragThresholdPx) {
    pointer_.box = true;
  }
  if (pointer_.box) {
    emit boxChanged();
  }
}

void EditorAiMaskTool::handleRelease(qreal x, qreal y, int /*button*/) {
  if (!pointer_.pressed) {
    return;
  }
  pointer_.last    = QPointF(x, y);
  const auto input = pointer_;
  pointer_         = {};
  emit boxChanged();
  if (busy()) {
    last_step_text_ = QStringLiteral("Wait until the current model request returns.");
    emit toolChanged();
    return;
  }
  if (interaction_ == nullptr) {
    Fail(QStringLiteral("The viewport is not bound to the AI Mask tool."));
    return;
  }
  const auto       mapping = interaction_->maskEditViewMapping();
  AiMaskPromptStep step;
  if (input.box) {
    const auto rect = MapItemRectToAiMaskPromptBox(mapping, input.press, input.last);
    if (!rect.has_value()) {
      last_step_text_ = QStringLiteral("The box is outside the photo.");
      emit toolChanged();
      return;
    }
    step.box = AiMaskBox{rect->x, rect->y, rect->x + rect->w, rect->y + rect->h};
  } else {
    const auto point = MapItemToAiMaskPromptPoint(mapping, input.last);
    if (!point.has_value()) {
      last_step_text_ = QStringLiteral("The click is outside the photo.");
      emit toolChanged();
      return;
    }
    step.points.push_back(AiMaskPromptPoint{point->x, point->y, AiMaskComponentOperation::Add});
  }
  const auto mode = subtract_ ? AiMaskComponentOperation::Subtract : AiMaskComponentOperation::Add;
  RunStep(input.box ? QStringLiteral("Running the box prompt...")
                    : QStringLiteral("Running the click prompt..."),
          [step = std::move(step), mode](AiMaskEditSession& session) {
            return session.AddPromptStep(step, mode);
          });
}

void EditorAiMaskTool::cancelPointer() {
  if (!pointer_.pressed) {
    return;
  }
  pointer_ = {};
  emit boxChanged();
}

// ── Reads ──────────────────────────────────────────────────────────────────

auto EditorAiMaskTool::input_mode() const -> QString {
  return view_.input_mode == AiMaskInputMode::Task ? QStringLiteral("task")
                                                   : QStringLiteral("prompt");
}

auto EditorAiMaskTool::preview_status() const -> QString {
  switch (preview_.status) {
    case EditorMaskCoveragePreviewStatus::Hidden:
      return QStringLiteral("Hidden");
    case EditorMaskCoveragePreviewStatus::Pending:
      return QStringLiteral("Pending");
    case EditorMaskCoveragePreviewStatus::Current:
      return QStringLiteral("Current");
    case EditorMaskCoveragePreviewStatus::Previous:
      return QStringLiteral("Previous");
  }
  return {};
}

auto EditorAiMaskTool::Backend() const -> IEditorSessionBackend* {
  return session_ != nullptr ? session_->session_backend() : nullptr;
}

auto EditorAiMaskTool::SelectedGradeId() const -> NodeId {
  auto* nodes = session_ != nullptr ? session_->node_selection_source() : nullptr;
  if (nodes == nullptr || nodes->selected_node_kind() != QLatin1String("colorGrade")) {
    return {};
  }
  return nodes->selected_node_id();
}

}  // namespace alcedo::ui
