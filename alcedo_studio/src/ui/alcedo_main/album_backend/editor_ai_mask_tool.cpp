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
#include <variant>

#include "app/ai_mask_model_output_store.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "sidecar_client/segmentation_client.hpp"
#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/edit_viewer/ai_mask_prompt_geometry.hpp"

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;

/// A press that moves less than this many item pixels before release is a click.
constexpr qreal    kDragThresholdPx       = 6.0;
/// Item distance between two recorded Path samples.
constexpr qreal    kPathSampleSpacingPx   = 2.0;
/// A cold sidecar start loads no CLIP model here, but the process start and the gRPC readiness
/// poll still need more than the default 5 s on a cold disk (same value as image analysis).
constexpr auto     kSidecarStartupTimeout = 60s;
/// Open stages: edit lease, analysis render, sidecar start, model load, image encoder.
constexpr int      kOpenStageCount        = 5;

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

[[nodiscard]] auto ToQRectF(const AiMaskBox& box) -> QRectF {
  return QRectF(QPointF(box.x0, box.y0), QPointF(box.x1, box.y1));
}

[[nodiscard]] auto ToQRectF(const SegmentationBox& box) -> QRectF {
  return QRectF(QPointF(box.x0, box.y0), QPointF(box.x1, box.y1)).normalized();
}

[[nodiscard]] auto ModelSetStateName(AiMaskModelSetAvailability availability) -> QString {
  switch (availability) {
    case AiMaskModelSetAvailability::kReady:
      return {};
    case AiMaskModelSetAvailability::kNotInstalled:
      return QStringLiteral("notInstalled");
    case AiMaskModelSetAvailability::kUpdateRequired:
      return QStringLiteral("updateRequired");
    case AiMaskModelSetAvailability::kNeedsNewerApplication:
      return QStringLiteral("needsNewerApplication");
    case AiMaskModelSetAvailability::kRepairRequired:
      return QStringLiteral("repairRequired");
  }
  return {};
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
    // The viewport lost the displayed coverage and the session presented it again (#514).
    coverage_again_connection_ = connect(
        session_, &EditorSessionController::MaskCoveragePresentedAgain, this,
        [this](EditorMaskCoverageResult result) { OnCoveragePresented(std::move(result)); });
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
  QObject::disconnect(coverage_again_connection_);
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

auto EditorAiMaskTool::model_set_state() const -> QString {
  return ModelSetStateName(model_set_availability_);
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
  active_                 = true;
  ready_                  = false;
  lease_open_             = false;
  commit_operation_id_    = 0;
  stage_                  = AiMaskToolStage::None;
  model_set_availability_ = AiMaskModelSetAvailability::kReady;
  error_text_.clear();
  notice_text_.clear();
  view_     = {};
  preview_  = {};
  pointer_  = {};
  pending_  = {};
  mask_id_  = std::move(mask_id);
  grade_id_ = {};
  emit toolChanged();
  emit messageChanged();
  emit previewChanged();
  emit busyChanged();
  emit liveInputChanged();
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
  // AI Models plan, AM5: only a complete installed set of a supported version loads. Otherwise
  // the panel shows the state and opens the AI Models page; no sidecar starts, no edit opens.
  const AiMaskModelSetReadiness model_set =
      services_.model_set ? services_.model_set() : AiMaskModelSetReadiness{};
  if (model_set.availability != AiMaskModelSetAvailability::kReady) {
    model_set_availability_ = model_set.availability;
    qInfo().noquote() << "[AI Mask] no usable model set:" << model_set_state();
    emit toolChanged();
    return;
  }
  // The range bars target the stored Mask of the edit; a new candidate has none (D11). They stay
  // disabled until the session opens the edit (OnOpened).
  session_->mask_creation()->SetAiMaskEditRangeTarget(true, {}, std::nullopt);
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
  request.model_directory = model_set.directory;

  SetStage(AiMaskToolStage::OpeningEdit);
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
                self->SetStage(AiMaskToolStage::RenderingAnalysis);
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
  if (session_ != nullptr) {
    session_->mask_creation()->SetAiMaskEditRangeTarget(true, grade_id_, mask_id_);
  }
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
  SetStage(AiMaskToolStage::StartingSidecar);
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

  PostStage(serial, AiMaskToolStage::LoadingModels);
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

  PostStage(serial, AiMaskToolStage::EncodingImage);
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
    // The analysis image guides the upscale of the S3OD alpha grids (master plan D23).
    if (analysis.image.width == started.image_width &&
        analysis.image.height == started.image_height && !analysis.image.pixels.empty()) {
      image.guide = std::make_shared<const AiMaskCoverageGuide>(
          MakeAiMaskCoverageGuide(image.extent, analysis.image.pixels));
    }
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

  qInfo().noquote() << QStringLiteral(
                           "[AI Mask] session started: %1 x %2, %3 on %4 (%5), sidecar %6 ms, "
                           "load %7 ms, image %8 ms (encoder %9 ms%10)")
                           .arg(started.image_width)
                           .arg(started.image_height)
                           .arg(ToQString(models.model_set_id), ToQString(models.backend),
                                ToQString(models.device))
                           .arg(sidecar_ms)
                           .arg(load_ms)
                           .arg(image_ms)
                           .arg(started.encoder_ms)
                           .arg(started.from_cache ? QStringLiteral(", cached") : QString{});
  auto view = ReadView(*worker_.session);
  QMetaObject::invokeMethod(
      this,
      [this, serial, view = std::move(view)]() mutable { OnStarted(serial, std::move(view)); },
      Qt::QueuedConnection);
}

void EditorAiMaskTool::OnStarted(std::uint64_t serial, SessionView view) {
  if (serial != serial_ || !active_) {
    return;
  }
  ready_ = true;
  view_  = std::move(view);
  emit toolChanged();
  SetStage(AiMaskToolStage::None);
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

void EditorAiMaskTool::setPromptTool(const QString& tool) {
  PromptTool next = PromptTool::Point;
  if (tool == QLatin1String("box")) {
    next = PromptTool::Box;
  } else if (tool == QLatin1String("path")) {
    next = PromptTool::Path;
  }
  if (next == prompt_tool_) {
    return;
  }
  cancelPointer();
  prompt_tool_ = next;
  emit toolChanged();
}

void EditorAiMaskTool::addTask(const QString& task) {
  const auto kind = TaskKindFromName(task);
  if (!kind.has_value()) {
    Fail(QStringLiteral("Unknown AI Mask task: %1").arg(task));
    return;
  }
  const auto operation =
      subtract_ ? AiMaskComponentOperation::Subtract : AiMaskComponentOperation::Add;
  RunStep(AiMaskToolStage::RunningModel, [kind = *kind, operation](AiMaskEditSession& session) {
    return session.AddTask(kind, operation);
  });
}

void EditorAiMaskTool::togglePerson(int person_index) {
  if (person_index < 0) {
    return;
  }
  RunStep(AiMaskToolStage::RunningModel,
          [index = static_cast<std::uint32_t>(person_index)](AiMaskEditSession& session) {
            return session.TogglePerson(index);
          });
}

void EditorAiMaskTool::highlightPerson(int person_index) {
  const int next = person_index >= 0 && person_index < view_.people.size() ? person_index : -1;
  if (next == highlighted_person_) {
    return;
  }
  highlighted_person_ = next;
  emit highlightChanged();
}

void EditorAiMaskTool::selectLevel(int level) {
  if (level < 0) {
    return;
  }
  RunStep(AiMaskToolStage::RunningModel,
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
  RunStep(AiMaskToolStage::Updating,
          [edge](AiMaskEditSession& session) { return session.SetEdgeAdjustment(edge); });
}

void EditorAiMaskTool::undo() {
  RunStep(AiMaskToolStage::Updating, [](AiMaskEditSession& session) {
    return session.Undo() ? AiMaskStepResult{}
                          : AiMaskStepResult{AiMaskStepStatus::Rejected, "Nothing to undo"};
  });
}

void EditorAiMaskTool::redo() {
  RunStep(AiMaskToolStage::Updating, [](AiMaskEditSession& session) {
    return session.Redo() ? AiMaskStepResult{}
                          : AiMaskStepResult{AiMaskStepStatus::Rejected, "Nothing to redo"};
  });
}

void EditorAiMaskTool::RunStep(AiMaskToolStage stage, SessionOperation operation) {
  if (!ready_ || busy()) {
    return;
  }
  ClearMessages();
  SetStage(stage);
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
    qInfo().noquote() << "[AI Mask] step" << StepResultText(result) << "in" << ElapsedMs(start)
                      << "ms";
    QMetaObject::invokeMethod(
        this,
        [this, serial, result, view = std::move(view)]() mutable {
          if (serial != serial_ || !active_) {
            return;
          }
          OnStepFinished(std::move(result), std::move(view));
        },
        Qt::QueuedConnection);
  });
}

void EditorAiMaskTool::OnStepFinished(AiMaskStepResult result, SessionView view) {
  switch (result.status) {
    case AiMaskStepStatus::Applied:
      view_ = std::move(view);
      break;
    case AiMaskStepStatus::Failed:
      error_text_ = ToQString(result.message);
      break;
    case AiMaskStepStatus::NotFound:
    case AiMaskStepStatus::Rejected:
      notice_text_ = ToQString(result.message);
      break;
  }
  ClearPending();
  emit toolChanged();
  emit messageChanged();
  SetStage(AiMaskToolStage::None);
  // A step that did not apply keeps the session coverage; a Feather or Expand preview of a slider
  // that the step did not accept returns to it.
  PresentCandidate(view_.candidate);
}

auto EditorAiMaskTool::ReadView(const AiMaskEditSession& session) -> SessionView {
  SessionView view;
  view.can_undo     = session.CanUndo();
  view.can_redo      = session.CanRedo();
  view.edge         = session.Edge();
  const auto& recipe = session.Recipe();
  const auto* last   = recipe.components.empty() ? nullptr : &recipe.components.back();
  const auto* group  = last != nullptr ? std::get_if<AiMaskPromptGroup>(&last->content) : nullptr;
  const auto* task   = last != nullptr ? std::get_if<AiMaskTaskComponent>(&last->content) : nullptr;
  if (group != nullptr && !group->steps.empty()) {
    view.level_count = static_cast<int>(AiMaskModelOutputStore::LevelCount(*group));
    if (view.level_count <= 1) {
      view.level_count = 0;
    }
    view.selected_level = static_cast<int>(group->steps.back().level);
  }
  // Markers of the open prompt group, with the Add or Subtract mode of their input (SM5 M3): the
  // first step of a group has the group operation, the later steps their point label.
  if (group != nullptr && session.PromptGroupOpen()) {
    for (std::size_t index = 0; index < group->steps.size(); ++index) {
      const auto& step = group->steps[index];
      const bool  adds = index == 0 ? last->operation == AiMaskComponentOperation::Add
                                    : step.points.empty() ||
                                         step.points.front().label == AiMaskComponentOperation::Add;
      QVariantMap marker;
      marker.insert(QStringLiteral("add"), adds);
      if (step.box.has_value()) {
        marker.insert(QStringLiteral("kind"), QStringLiteral("box"));
        marker.insert(QStringLiteral("box"), ToQRectF(*step.box));
      } else {
        QVariantList points;
        for (const auto& point : step.points) {
          points.push_back(QPointF(point.x, point.y));
        }
        marker.insert(QStringLiteral("kind"),
                      step.points.size() > 1 ? QStringLiteral("path") : QStringLiteral("point"));
        marker.insert(QStringLiteral("points"), points);
      }
      view.prompt_steps.push_back(marker);
    }
  }
  view.people_active = task != nullptr && task->task == AiMaskTaskKind::People;
  const auto people  = session.People();
  for (std::size_t index = 0; index < people.size(); ++index) {
    const bool  included = session.PersonIncluded(static_cast<std::uint32_t>(index));
    const auto  box      = ToQRectF(people[index].box);
    QVariantMap person;
    person.insert(QStringLiteral("index"), static_cast<int>(index));
    person.insert(QStringLiteral("score"), people[index].score);
    person.insert(QStringLiteral("included"), included);
    person.insert(QStringLiteral("box"), box);
    view.people.push_back(person);
    if (view.people_active && included) {
      view.selected_person_boxes.push_back(box);
    }
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
  } else if (session_ != nullptr) {
    // The candidate replaces the range coverage of the stored Mask (D11).
    session_->mask_creation()->OnRangeCoverageReplaced();
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
  ClearMessages();
  SetStage(AiMaskToolStage::Committing);
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
  active_                 = false;
  ready_                  = false;
  lease_open_             = false;
  commit_operation_id_    = 0;
  stage_                  = AiMaskToolStage::None;
  model_set_availability_ = AiMaskModelSetAvailability::kReady;
  error_text_             = std::move(error);
  notice_text_.clear();
  view_               = {};
  preview_            = {};
  pointer_            = {};
  pending_            = {};
  highlighted_person_ = -1;
  mask_id_.reset();
  if (session_ != nullptr) {
    session_->mask_creation()->SetAiMaskEditRangeTarget(false, {}, std::nullopt);
  }
  emit toolChanged();
  emit highlightChanged();
  emit messageChanged();
  emit previewChanged();
  emit busyChanged();
  emit liveInputChanged();
}

void EditorAiMaskTool::Fail(QString error) {
  qWarning().noquote() << "[AI Mask]" << error;
  error_text_ = std::move(error);
  ClearPending();
  emit messageChanged();
  SetStage(AiMaskToolStage::None);
}

void EditorAiMaskTool::SetStage(AiMaskToolStage stage) {
  if (stage_ == stage) {
    return;
  }
  stage_ = stage;
  emit busyChanged();
}

void EditorAiMaskTool::PostStage(std::uint64_t serial, AiMaskToolStage stage) {
  QMetaObject::invokeMethod(
      this,
      [this, serial, stage] {
        if (serial == serial_ && active_ && !ready_ && busy()) {
          SetStage(stage);
        }
      },
      Qt::QueuedConnection);
}

void EditorAiMaskTool::SetNotice(QString notice) {
  notice_text_ = std::move(notice);
  emit messageChanged();
}

void EditorAiMaskTool::ClearMessages() {
  if (error_text_.isEmpty() && notice_text_.isEmpty()) {
    return;
  }
  error_text_.clear();
  notice_text_.clear();
  emit messageChanged();
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
  const QPointF item(x, y);
  pointer_ = PointerInput{true, false, item, item, {item}};
  if (!busy()) {
    ClearPending();
  }
  if (prompt_tool_ == PromptTool::Path && !busy()) {
    emit liveInputChanged();
  }
  return true;
}

void EditorAiMaskTool::handleMove(qreal x, qreal y, int /*buttons*/) {
  if (!pointer_.pressed) {
    return;
  }
  const QPointF item(x, y);
  pointer_.last = item;
  if (!pointer_.dragging && QLineF(pointer_.press, item).length() > kDragThresholdPx) {
    pointer_.dragging = true;
  }
  if (prompt_tool_ == PromptTool::Path) {
    if (QLineF(pointer_.path.back(), item).length() >= kPathSampleSpacingPx) {
      pointer_.path.push_back(item);
      emit liveInputChanged();
    }
  } else if (prompt_tool_ == PromptTool::Box && pointer_.dragging) {
    emit liveInputChanged();
  }
}

void EditorAiMaskTool::handleRelease(qreal x, qreal y, int /*button*/) {
  if (!pointer_.pressed) {
    return;
  }
  pointer_.last = QPointF(x, y);
  if (prompt_tool_ == PromptTool::Path && pointer_.path.back() != pointer_.last) {
    pointer_.path.push_back(pointer_.last);
  }
  const auto input = std::move(pointer_);
  pointer_         = {};
  emit liveInputChanged();
  if (busy()) {
    SetNotice(QStringLiteral("Wait until the current model request returns."));
    return;
  }
  RunPointerInput(input);
}

void EditorAiMaskTool::RunPointerInput(const PointerInput& input) {
  if (interaction_ == nullptr) {
    Fail(QStringLiteral("The viewport is not bound to the AI Mask tool."));
    return;
  }
  const auto       mapping = interaction_->maskEditViewMapping();
  AiMaskPromptStep step;
  auto             start = AiMaskPromptGroupStart::NewGroup;
  PendingInput     pending;
  pending.visible = true;
  pending.tool    = prompt_tool_;
  pending.add     = !subtract_;
  switch (prompt_tool_) {
    case PromptTool::Point: {
      // A click refines the open prompt group (SM5 M3): Add and Subtract are its point labels.
      const auto point = MapItemToAiMaskPromptPoint(mapping, input.press);
      if (!point.has_value()) {
        SetNotice(QStringLiteral("The click is outside the photo."));
        return;
      }
      step.points.push_back(AiMaskPromptPoint{point->x, point->y, AiMaskComponentOperation::Add});
      start = AiMaskPromptGroupStart::JoinOpenGroup;
      pending.points.push_back(input.press);
      break;
    }
    case PromptTool::Box: {
      if (!input.dragging) {
        SetNotice(QStringLiteral("Drag a box around the object."));
        return;
      }
      const auto rect = MapItemRectToAiMaskPromptBox(mapping, input.press, input.last);
      if (!rect.has_value()) {
        SetNotice(QStringLiteral("The box is outside the photo."));
        return;
      }
      step.box    = AiMaskBox{rect->x, rect->y, rect->x + rect->w, rect->y + rect->h};
      pending.box = QRectF(input.press, input.last).normalized();
      break;
    }
    case PromptTool::Path: {
      const auto points =
          MapItemStrokeToAiMaskPromptPoints(mapping, input.path, kAiMaskMaximumStrokePromptPoints);
      if (points.empty()) {
        SetNotice(QStringLiteral("The path is outside the photo."));
        return;
      }
      for (const auto& point : points) {
        step.points.push_back(AiMaskPromptPoint{point.x, point.y, AiMaskComponentOperation::Add});
      }
      pending.points = input.path;
      break;
    }
  }
  pending_ = std::move(pending);
  emit       liveInputChanged();
  const auto mode = subtract_ ? AiMaskComponentOperation::Subtract : AiMaskComponentOperation::Add;
  RunStep(AiMaskToolStage::RunningModel,
          [step = std::move(step), mode, start](AiMaskEditSession& session) {
            return session.AddPromptStep(step, mode, start);
          });
}

void EditorAiMaskTool::cancelPointer() {
  if (!pointer_.pressed) {
    return;
  }
  pointer_ = {};
  emit liveInputChanged();
}

void EditorAiMaskTool::ClearPending() {
  if (!pending_.visible) {
    return;
  }
  pending_ = {};
  emit liveInputChanged();
}

// ── Reads ──────────────────────────────────────────────────────────────────

auto EditorAiMaskTool::live_input() const -> QVariantMap {
  QVariantMap input;
  const bool  add = !subtract_;
  if (pointer_.pressed) {
    if (prompt_tool_ == PromptTool::Path) {
      QVariantList points;
      for (const auto& point : pointer_.path) {
        points.push_back(point);
      }
      input.insert(QStringLiteral("kind"), QStringLiteral("path"));
      input.insert(QStringLiteral("points"), points);
    } else if (prompt_tool_ == PromptTool::Box && pointer_.dragging) {
      input.insert(QStringLiteral("kind"), QStringLiteral("box"));
      input.insert(QStringLiteral("box"), QRectF(pointer_.press, pointer_.last).normalized());
    } else {
      return {};
    }
    input.insert(QStringLiteral("add"), add);
    return input;
  }
  if (!pending_.visible) {
    return {};
  }
  switch (pending_.tool) {
    case PromptTool::Point:
    case PromptTool::Path: {
      QVariantList points;
      for (const auto& point : pending_.points) {
        points.push_back(point);
      }
      input.insert(QStringLiteral("kind"), pending_.tool == PromptTool::Path
                                               ? QStringLiteral("path")
                                               : QStringLiteral("point"));
      input.insert(QStringLiteral("points"), points);
      break;
    }
    case PromptTool::Box:
      input.insert(QStringLiteral("kind"), QStringLiteral("box"));
      input.insert(QStringLiteral("box"), pending_.box);
      break;
  }
  input.insert(QStringLiteral("add"), pending_.add);
  return input;
}

auto EditorAiMaskTool::highlighted_person_box() const -> QRectF {
  if (highlighted_person_ < 0 || highlighted_person_ >= view_.people.size()) {
    return {};
  }
  return view_.people[highlighted_person_].toMap().value(QStringLiteral("box")).toRectF();
}

auto EditorAiMaskTool::prompt_tool() const -> QString {
  switch (prompt_tool_) {
    case PromptTool::Point:
      return QStringLiteral("point");
    case PromptTool::Box:
      return QStringLiteral("box");
    case PromptTool::Path:
      return QStringLiteral("path");
  }
  return {};
}

auto EditorAiMaskTool::busy_stage() const -> QString {
  switch (stage_) {
    case AiMaskToolStage::None:
      return {};
    case AiMaskToolStage::OpeningEdit:
      return QStringLiteral("openingEdit");
    case AiMaskToolStage::RenderingAnalysis:
      return QStringLiteral("renderingAnalysis");
    case AiMaskToolStage::StartingSidecar:
      return QStringLiteral("startingSidecar");
    case AiMaskToolStage::LoadingModels:
      return QStringLiteral("loadingModels");
    case AiMaskToolStage::EncodingImage:
      return QStringLiteral("encodingImage");
    case AiMaskToolStage::RunningModel:
      return QStringLiteral("runningModel");
    case AiMaskToolStage::Updating:
      return QStringLiteral("updating");
    case AiMaskToolStage::Committing:
      return QStringLiteral("committing");
  }
  return {};
}

auto EditorAiMaskTool::busy_step() const -> int {
  switch (stage_) {
    case AiMaskToolStage::OpeningEdit:
      return 1;
    case AiMaskToolStage::RenderingAnalysis:
      return 2;
    case AiMaskToolStage::StartingSidecar:
      return 3;
    case AiMaskToolStage::LoadingModels:
      return 4;
    case AiMaskToolStage::EncodingImage:
      return 5;
    default:
      return 0;
  }
}

auto EditorAiMaskTool::busy_step_count() const -> int {
  return busy_step() > 0 ? kOpenStageCount : 0;
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
