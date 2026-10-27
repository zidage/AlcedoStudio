//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/ai_mask_edit_session.hpp"
#include "app/ai_mask_segmentation_service.hpp"
#include "app/ai_sidecar_runtime_service.hpp"
#include "app/editor_mask_coverage_preview.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/mask_coverage_selection.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"

namespace alcedo::ui {

/**
 * @brief AI Mask tool of the editor (SM9 MVP, issue #513; tool controls, issue #519).
 *
 * Connects the production owners in the running editor: the sidecar segmentation service and the
 * AiMaskEditSession on one serial worker thread, the EditorSessionService AI Mask facade (Open,
 * candidate coverage, Retry, Commit, Cancel), and the main viewport coverage display. The tool
 * keeps no second renderer, stack, or coverage copy: QML reads focused values that the worker
 * reads from the session after each step.
 *
 * Input: the selected prompt tool takes the left button in the viewport. Point adds a click to
 * the open prompt group with the Add or Subtract label; Box and Path start a new group with the
 * dragged box or the released stroke. All map through the SM4 prompt geometry to normalized
 * ReferenceSpace. Tasks and person toggles close the group (SM5 M3).
 *
 * Progress: a running operation reports its stage (@ref busyStage). The Open stages count from 1
 * to @ref busyStepCount; other operations have no step count.
 *
 * Threading: the GUI thread calls every Q_INVOKABLE and reads every property. The segmentation
 * service and the session live on the worker; facade completions arrive on the session owner
 * thread and are posted to the GUI thread.
 */
class EditorAiMaskTool final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY toolChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY toolChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  /// Key of the running stage: "openingEdit", "renderingAnalysis", "startingSidecar",
  /// "loadingModels", "encodingImage", "runningModel", "updating", "committing", or "".
  Q_PROPERTY(QString busyStage READ busy_stage NOTIFY busyChanged)
  /// 1-based Open stage; 0 for an operation without stages.
  Q_PROPERTY(int busyStep READ busy_step NOTIFY busyChanged)
  Q_PROPERTY(int busyStepCount READ busy_step_count NOTIFY busyChanged)
  Q_PROPERTY(QString errorText READ error_text NOTIFY messageChanged)
  /// Why the last input made no step (Rejected or NotFound); not an error.
  Q_PROPERTY(QString noticeText READ notice_text NOTIFY messageChanged)
  Q_PROPERTY(bool ownsLeftButton READ owns_left_button NOTIFY toolChanged)
  Q_PROPERTY(bool editingStoredMask READ editing_stored_mask NOTIFY toolChanged)
  Q_PROPERTY(bool subtract READ subtract WRITE setSubtract NOTIFY toolChanged)
  /// "point", "box", or "path".
  Q_PROPERTY(QString promptTool READ prompt_tool WRITE setPromptTool NOTIFY toolChanged)
  Q_PROPERTY(bool canUndo READ can_undo NOTIFY toolChanged)
  Q_PROPERTY(bool canRedo READ can_redo NOTIFY toolChanged)
  Q_PROPERTY(qreal featherPx READ feather_px NOTIFY toolChanged)
  Q_PROPERTY(int expandPx READ expand_px NOTIFY toolChanged)
  Q_PROPERTY(qreal maximumFeatherPx READ maximum_feather_px CONSTANT)
  Q_PROPERTY(int maximumExpandPx READ maximum_expand_px CONSTANT)
  /// Levels of the last prompt group: 3 for a single click without a box, else 0.
  Q_PROPERTY(int levelCount READ level_count NOTIFY toolChanged)
  Q_PROPERTY(int selectedLevel READ selected_level NOTIFY toolChanged)
  /// People list: maps with index, score, included, and box (normalized QRectF).
  Q_PROPERTY(QVariantList people READ people NOTIFY toolChanged)
  /// The last component is People, so a person toggle applies.
  Q_PROPERTY(bool peopleActive READ people_active NOTIFY toolChanged)
  /// Normalized boxes of the included people while People is active.
  Q_PROPERTY(QVariantList selectedPersonBoxes READ selected_person_boxes NOTIFY toolChanged)
  /// Normalized box of the person under the pointer in the People list; empty for none.
  Q_PROPERTY(QRectF highlightedPersonBox READ highlighted_person_box NOTIFY highlightChanged)
  /// Steps of the open prompt group for the viewport markers (AiMaskInputOverlayItem map
  /// format, normalized analysis image coordinates).
  Q_PROPERTY(QVariantList promptSteps READ prompt_steps NOTIFY toolChanged)
  Q_PROPERTY(QString previewStatus READ preview_status NOTIFY previewChanged)
  Q_PROPERTY(QString previewError READ preview_error NOTIFY previewChanged)
  /// The input that the pointer draws, or that waits for its model step (item coordinates,
  /// AiMaskInputOverlayItem map format); empty when there is none.
  Q_PROPERTY(QVariantMap liveInput READ live_input NOTIFY liveInputChanged)

 public:
  /// Production services read on the GUI thread at Open.
  struct Services {
    /// The sidecar runtime of the open project; null without a project.
    std::function<std::shared_ptr<AiSidecarRuntimeService>()> runtime;
    /// Start options of the sidecar process.
    std::function<AiSidecarRuntimeOptions()>                  runtime_options;
    /// `<model download directory>/alcedo-ai-mask-v1` (SM3.A section 2.1).
    std::function<std::filesystem::path()>                    model_directory;
  };

  EditorAiMaskTool(EditorSessionController* session, Services services, QObject* parent = nullptr);
  ~EditorAiMaskTool() override;

  EditorAiMaskTool(const EditorAiMaskTool&)                                  = delete;
  auto               operator=(const EditorAiMaskTool&) -> EditorAiMaskTool& = delete;

  /// Cancels the open edit and drains the worker (models unload). Idempotent.
  void               Shutdown();

  /// Opens a new AI Mask on the selected Color Grade.
  Q_INVOKABLE void   open();
  /// Opens the AI Mask that the Mask list selects (decision M15: stored coverage, no model call).
  /// Only a user action opens it: the "Edit AI Mask" button or the header AI Mask button; a row
  /// selection only selects the Mask (#564).
  Q_INVOKABLE void          openSelectedMask();
  Q_INVOKABLE void   bindInteractionItem(QObject* interaction);
  Q_INVOKABLE void   setSubtract(bool subtract);
  /// "point", "box", or "path".
  Q_INVOKABLE void          setPromptTool(const QString& tool);
  /// "subject", "background", "sky", or "people".
  Q_INVOKABLE void   addTask(const QString& task);
  Q_INVOKABLE void   togglePerson(int person_index);
  /// Outlines person @p person_index in the viewport while the People list row is hovered; -1
  /// clears it. Presentation only.
  Q_INVOKABLE void          highlightPerson(int person_index);
  Q_INVOKABLE void   selectLevel(int level);
  /// Shows Feather and Expand while a slider moves; adds no session step.
  Q_INVOKABLE void   previewEdge(qreal feather_px, int expand_px);
  /// Adds one session edge step for the released slider value.
  Q_INVOKABLE void   acceptEdge(qreal feather_px, int expand_px);
  Q_INVOKABLE void   undo();
  Q_INVOKABLE void   redo();
  Q_INVOKABLE void   retryPreview();
  Q_INVOKABLE void   commit();
  /// Ends the edit without an asset or history write. Also closes the tool after an error.
  Q_INVOKABLE void   cancel();
  Q_INVOKABLE bool   handlePress(qreal x, qreal y, int button);
  Q_INVOKABLE void   handleMove(qreal x, qreal y, int buttons);
  Q_INVOKABLE void   handleRelease(qreal x, qreal y, int button);
  Q_INVOKABLE void   cancelPointer();

  [[nodiscard]] auto active() const -> bool { return active_; }
  [[nodiscard]] auto ready() const -> bool { return ready_; }
  [[nodiscard]] auto        busy() const -> bool { return stage_ != AiMaskToolStage::None; }
  [[nodiscard]] auto        busy_stage() const -> QString;
  [[nodiscard]] auto        busy_step() const -> int;
  [[nodiscard]] auto        busy_step_count() const -> int;
  [[nodiscard]] auto error_text() const -> QString { return error_text_; }
  [[nodiscard]] auto        notice_text() const -> QString { return notice_text_; }
  [[nodiscard]] auto        owns_left_button() const -> bool { return active_ && ready_; }
  [[nodiscard]] auto        editing_stored_mask() const -> bool { return mask_id_.has_value(); }
  [[nodiscard]] auto        subtract() const -> bool { return subtract_; }
  [[nodiscard]] auto        prompt_tool() const -> QString;
  [[nodiscard]] auto        can_undo() const -> bool { return view_.can_undo; }
  [[nodiscard]] auto        can_redo() const -> bool { return view_.can_redo; }
  [[nodiscard]] auto        feather_px() const -> qreal { return view_.edge.feather_px; }
  [[nodiscard]] auto        expand_px() const -> int { return view_.edge.expand_px; }
  [[nodiscard]] static auto maximum_feather_px() -> qreal { return kAiMaskMaximumFeatherPx; }
  [[nodiscard]] static auto maximum_expand_px() -> int { return kAiMaskMaximumExpandPx; }
  [[nodiscard]] auto        level_count() const -> int { return view_.level_count; }
  [[nodiscard]] auto        selected_level() const -> int { return view_.selected_level; }
  [[nodiscard]] auto        people() const -> QVariantList { return view_.people; }
  [[nodiscard]] auto        people_active() const -> bool { return view_.people_active; }
  [[nodiscard]] auto        selected_person_boxes() const -> QVariantList {
    return view_.selected_person_boxes;
  }
  [[nodiscard]] auto prompt_steps() const -> QVariantList { return view_.prompt_steps; }
  [[nodiscard]] auto highlighted_person_box() const -> QRectF;
  [[nodiscard]] auto preview_status() const -> QString;
  [[nodiscard]] auto preview_error() const -> QString {
    return QString::fromStdString(preview_.error);
  }
  [[nodiscard]] auto live_input() const -> QVariantMap;

 signals:
  void toolChanged();
  /// The coverage preview state changed. Separate from toolChanged, so a Feather or Expand
  /// preview during a slider drag does not rebind the slider values.
  void previewChanged();
  /// The error or notice text changed. Separate from toolChanged for the same reason.
  void messageChanged();
  void busyChanged();
  void liveInputChanged();
  void highlightChanged();

 private:
  /// The operation that runs (SM9 progress display; there is no busy text).
  enum class AiMaskToolStage : std::uint8_t {
    None,
    OpeningEdit,
    RenderingAnalysis,
    StartingSidecar,
    LoadingModels,
    EncodingImage,
    RunningModel,
    Updating,
    Committing,
  };

  enum class PromptTool : std::uint8_t { Point, Box, Path };

  /// Values of the current session state, read on the worker after each step.
  struct SessionView {
    bool                           can_undo = false;
    bool                           can_redo = false;
    AiMaskEdgeAdjustment           edge{};
    int                            level_count    = 0;
    int                            selected_level = 0;
    QVariantList                   people;
    bool                           people_active = false;
    QVariantList                   selected_person_boxes;
    QVariantList                   prompt_steps;
    /// Shared read of the current coverage (AcquireCoveragePixels); never copied.
    MaskCoverageCandidateSelection candidate{};
  };

  struct PointerInput {
    bool                 pressed  = false;
    /// Moved past the drag threshold.
    bool                 dragging = false;
    QPointF              press;
    QPointF              last;
    /// Path samples in item coordinates.
    std::vector<QPointF> path;
  };

  /// The released input that waits for its model step; the viewport shows it until the step
  /// returns.
  struct PendingInput {
    bool                 visible = false;
    PromptTool           tool    = PromptTool::Point;
    bool                 add     = true;
    std::vector<QPointF> points;
    QRectF               box;
  };

  /// Worker-thread state: the segmentation service and the session that calls it.
  struct Worker {
    std::unique_ptr<AiMaskSegmentationService> service;
    std::unique_ptr<AiMaskEditSession>         session;
  };

  /// Inputs of the worker start after the analysis render.
  struct StartRequest {
    std::uint64_t                            serial = 0;
    std::shared_ptr<AiSidecarRuntimeService> runtime;
    AiSidecarRuntimeOptions                  runtime_options;
    std::filesystem::path                    model_directory;
    std::string                              geometry_fingerprint;
    std::optional<AiSegmentationMaskSource>  stored_source;
    std::optional<MaskAsset>                 stored_asset;
  };

  using SessionOperation = std::function<AiMaskStepResult(AiMaskEditSession& session)>;

  void OpenFor(std::optional<MaskId> mask_id);
  /// Shows the tool with @p error and no open edit, so Close dismisses it.
  void ShowError(QString error);
  void OnOpened(std::uint64_t serial, EditorAiMaskEditOpenResult opened);
  void OnAnalysisRendered(std::uint64_t serial, EditorAiMaskAnalysisResult analysis,
                          StartRequest request);
  /// Runs on the worker: starts the sidecar, loads the models, starts the image, makes the session.
  void StartOnWorker(EditorAiMaskAnalysisResult analysis, StartRequest request);
  void               OnStarted(std::uint64_t serial, SessionView view);
  /// Runs @p operation on the worker session, then presents the new coverage.
  void               RunStep(AiMaskToolStage stage, SessionOperation operation);
  void               OnStepFinished(AiMaskStepResult result, SessionView view);
  void PresentCandidate(const MaskCoverageCandidateSelection& candidate);
  void OnCoveragePresented(EditorMaskCoverageResult result);
  void OnSessionResult(const EditorSessionResult& result);
  void OnSessionStateChanged();
  /// Ends the tool: unloads the models on the worker and clears the GUI state.
  void Close(QString error);
  void Fail(QString error);
  void               SetStage(AiMaskToolStage stage);
  /// Sets @p stage of the Open with @p serial from the worker thread.
  void               PostStage(std::uint64_t serial, AiMaskToolStage stage);
  void               SetNotice(QString notice);
  void               ClearMessages();
  /// Turns the released pointer input into a prompt step, or a notice when it has none.
  void               RunPointerInput(const PointerInput& input);
  void               ClearPending();
  void PostToWorker(std::function<void(Worker& worker)> task);
  [[nodiscard]] auto Backend() const -> IEditorSessionBackend*;
  [[nodiscard]] auto SelectedGradeId() const -> NodeId;
  [[nodiscard]] static auto         ReadView(const AiMaskEditSession& session) -> SessionView;
  [[nodiscard]] static auto         StepResultText(const AiMaskStepResult& result) -> QString;

  QPointer<EditorSessionController> session_;
  QPointer<editor_rhi::EditorInteractionController> interaction_;
  Services                                          services_;
  QThread                                           worker_thread_;
  /// Lives on @ref worker_thread_; the queued tasks run in its event loop.
  QObject*                                          worker_context_ = nullptr;
  /// Touched only on the worker thread.
  Worker                                            worker_;
  bool                                              shut_down_  = false;

  /// One serial per Open. A worker or facade result of an earlier Open (Cancel, then Open while
  /// the earlier start still runs on the worker) carries the earlier serial and is ignored.
  std::uint64_t                                     serial_     = 0;
  bool                                              active_     = false;
  bool                                              ready_      = false;
  /// The AiMaskEdit lease of EditorSessionService is held.
  bool                                              lease_open_ = false;
  NodeId                                            grade_id_;
  std::optional<MaskId>                             mask_id_;
  AiMaskToolStage                                   stage_ = AiMaskToolStage::None;
  QString                                           error_text_;
  QString                                           notice_text_;
  bool                                              subtract_    = false;
  PromptTool                                        prompt_tool_ = PromptTool::Point;
  SessionView                                       view_;
  EditorMaskCoveragePreviewState                    preview_;
  /// Operation id of the queued Commit; 0 when no Commit waits for its result.
  std::uint64_t                                     commit_operation_id_ = 0;
  PointerInput                                      pointer_;
  PendingInput                                      pending_;
  /// People list index under the pointer; -1 for none.
  int                                               highlighted_person_ = -1;
  QMetaObject::Connection                           result_connection_;
  QMetaObject::Connection                           state_connection_;
};

}  // namespace alcedo::ui
