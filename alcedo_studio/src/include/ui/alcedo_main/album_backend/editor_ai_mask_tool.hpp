//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QVariantList>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
 * @brief AI Mask tool of the editor (SM9 MVP, issue #513).
 *
 * Connects the production owners in the running editor: the sidecar segmentation service and the
 * AiMaskEditSession on one serial worker thread, the EditorSessionService AI Mask facade (Open,
 * candidate coverage, Retry, Commit, Cancel), and the main viewport coverage display. The tool
 * keeps no second renderer, stack, or coverage copy: QML reads focused values that the worker
 * projects after each session step.
 *
 * Input: a left click in the viewport adds a click prompt, a left drag adds a box prompt. Both
 * map through the SM4 prompt geometry to normalized ReferenceSpace.
 *
 * Threading: the GUI thread calls every Q_INVOKABLE and reads every property. The segmentation
 * service and the session live on the worker; facade completions arrive on the session owner
 * thread and are posted to the GUI thread.
 */
class EditorAiMaskTool final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY toolChanged)
  Q_PROPERTY(bool ready READ ready NOTIFY toolChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY toolChanged)
  Q_PROPERTY(QString busyText READ busy_text NOTIFY toolChanged)
  Q_PROPERTY(QString errorText READ error_text NOTIFY toolChanged)
  Q_PROPERTY(bool ownsLeftButton READ owns_left_button NOTIFY toolChanged)
  Q_PROPERTY(bool editingStoredMask READ editing_stored_mask NOTIFY toolChanged)
  Q_PROPERTY(bool subtract READ subtract WRITE setSubtract NOTIFY toolChanged)
  Q_PROPERTY(QString inputMode READ input_mode NOTIFY toolChanged)
  Q_PROPERTY(bool canUndo READ can_undo NOTIFY toolChanged)
  Q_PROPERTY(bool canRedo READ can_redo NOTIFY toolChanged)
  Q_PROPERTY(int stateCount READ state_count NOTIFY toolChanged)
  Q_PROPERTY(qreal featherPx READ feather_px NOTIFY toolChanged)
  Q_PROPERTY(int expandPx READ expand_px NOTIFY toolChanged)
  Q_PROPERTY(qreal maximumFeatherPx READ maximum_feather_px CONSTANT)
  Q_PROPERTY(int maximumExpandPx READ maximum_expand_px CONSTANT)
  Q_PROPERTY(QVariantList people READ people NOTIFY toolChanged)
  Q_PROPERTY(QString imageText READ image_text NOTIFY toolChanged)
  Q_PROPERTY(QString lastStepText READ last_step_text NOTIFY toolChanged)
  Q_PROPERTY(QString previewStatus READ preview_status NOTIFY previewChanged)
  Q_PROPERTY(QString previewError READ preview_error NOTIFY previewChanged)
  Q_PROPERTY(bool boxVisible READ box_visible NOTIFY boxChanged)
  Q_PROPERTY(qreal boxX READ box_x NOTIFY boxChanged)
  Q_PROPERTY(qreal boxY READ box_y NOTIFY boxChanged)
  Q_PROPERTY(qreal boxWidth READ box_width NOTIFY boxChanged)
  Q_PROPERTY(qreal boxHeight READ box_height NOTIFY boxChanged)

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
  Q_INVOKABLE void   openSelectedMask();
  Q_INVOKABLE void   bindInteractionItem(QObject* interaction);
  Q_INVOKABLE void   setSubtract(bool subtract);
  /// "prompt" or "task".
  Q_INVOKABLE void   setInputMode(const QString& mode);
  /// "subject", "background", "sky", or "people".
  Q_INVOKABLE void   addTask(const QString& task);
  Q_INVOKABLE void   togglePerson(int person_index);
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
  [[nodiscard]] auto busy() const -> bool { return !busy_text_.isEmpty(); }
  [[nodiscard]] auto busy_text() const -> QString { return busy_text_; }
  [[nodiscard]] auto error_text() const -> QString { return error_text_; }
  [[nodiscard]] auto owns_left_button() const -> bool {
    return active_ && ready_ && view_.input_mode == AiMaskInputMode::Prompt;
  }
  [[nodiscard]] auto        editing_stored_mask() const -> bool { return mask_id_.has_value(); }
  [[nodiscard]] auto        subtract() const -> bool { return subtract_; }
  [[nodiscard]] auto        input_mode() const -> QString;
  [[nodiscard]] auto        can_undo() const -> bool { return view_.can_undo; }
  [[nodiscard]] auto        can_redo() const -> bool { return view_.can_redo; }
  [[nodiscard]] auto        state_count() const -> int { return view_.state_count; }
  [[nodiscard]] auto        feather_px() const -> qreal { return view_.edge.feather_px; }
  [[nodiscard]] auto        expand_px() const -> int { return view_.edge.expand_px; }
  [[nodiscard]] static auto maximum_feather_px() -> qreal { return kAiMaskMaximumFeatherPx; }
  [[nodiscard]] static auto maximum_expand_px() -> int { return kAiMaskMaximumExpandPx; }
  [[nodiscard]] auto        people() const -> QVariantList { return view_.people; }
  [[nodiscard]] auto        image_text() const -> QString { return image_text_; }
  [[nodiscard]] auto        last_step_text() const -> QString { return last_step_text_; }
  [[nodiscard]] auto        preview_status() const -> QString;
  [[nodiscard]] auto        preview_error() const -> QString {
    return QString::fromStdString(preview_.error);
  }
  [[nodiscard]] auto box_visible() const -> bool { return pointer_.box; }
  [[nodiscard]] auto box_x() const -> qreal {
    return std::min(pointer_.press.x(), pointer_.last.x());
  }
  [[nodiscard]] auto box_y() const -> qreal {
    return std::min(pointer_.press.y(), pointer_.last.y());
  }
  [[nodiscard]] auto box_width() const -> qreal {
    return std::abs(pointer_.last.x() - pointer_.press.x());
  }
  [[nodiscard]] auto box_height() const -> qreal {
    return std::abs(pointer_.last.y() - pointer_.press.y());
  }

 signals:
  void toolChanged();
  /// The coverage preview state changed. Separate from toolChanged, so a Feather or Expand
  /// preview during a slider drag does not rebind the slider values.
  void previewChanged();
  void boxChanged();

 private:
  /// Values of the current session state, read on the worker after each step.
  struct SessionView {
    bool                           can_undo    = false;
    bool                           can_redo    = false;
    int                            state_count = 0;
    AiMaskInputMode                input_mode  = AiMaskInputMode::Prompt;
    AiMaskEdgeAdjustment           edge{};
    QVariantList                   people;
    /// Shared read of the current coverage (AcquireCoveragePixels); never copied.
    MaskCoverageCandidateSelection candidate{};
  };

  struct PointerInput {
    bool    pressed = false;
    bool    box     = false;
    QPointF press;
    QPointF last;
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
  void OnStarted(std::uint64_t serial, QString image_text, SessionView view);
  /// Runs @p operation on the worker session, then presents the new coverage.
  void RunStep(QString busy_text, SessionOperation operation);
  void OnStepFinished(std::uint64_t serial, AiMaskStepResult result, SessionView view);
  void PresentCandidate(const MaskCoverageCandidateSelection& candidate);
  void OnCoveragePresented(EditorMaskCoverageResult result);
  void OnSessionResult(const EditorSessionResult& result);
  void OnSessionStateChanged();
  /// Ends the tool: unloads the models on the worker and clears the GUI state.
  void Close(QString error);
  void Fail(QString error);
  void SetBusy(QString text);
  void PostToWorker(std::function<void(Worker& worker)> task);
  [[nodiscard]] auto                Backend() const -> IEditorSessionBackend*;
  [[nodiscard]] auto                SelectedGradeId() const -> NodeId;
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
  QString                                           busy_text_;
  QString                                           error_text_;
  QString                                           image_text_;
  QString                                           last_step_text_;
  bool                                              subtract_ = false;
  SessionView                                       view_;
  EditorMaskCoveragePreviewState                    preview_;
  /// Operation id of the queued Commit; 0 when no Commit waits for its result.
  std::uint64_t                                     commit_operation_id_ = 0;
  PointerInput                                      pointer_;
  QMetaObject::Connection                           result_connection_;
  QMetaObject::Connection                           state_connection_;
};

}  // namespace alcedo::ui
