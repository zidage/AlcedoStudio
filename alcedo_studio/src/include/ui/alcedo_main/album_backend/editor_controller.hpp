//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/alcedo_main/i18n.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "app/pipeline_service.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"

namespace alcedo::ui {

class LibraryModule;
class ProjectModule;

/// Manages the inline image editor (pipeline-based adjustments + preview).
class EditorController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool editorActive READ EditorActive NOTIFY EditorStateChanged)
  Q_PROPERTY(bool editorBusy READ EditorBusy NOTIFY EditorStateChanged)
  Q_PROPERTY(uint editorElementId READ EditorElementId NOTIFY EditorStateChanged)
  Q_PROPERTY(QString editorTitle READ EditorTitle NOTIFY EditorStateChanged)
  Q_PROPERTY(QString editorStatus READ EditorStatus NOTIFY EditorStateChanged)
  Q_PROPERTY(QString editorPreviewUrl READ EditorPreviewUrl NOTIFY EditorPreviewChanged)
  Q_PROPERTY(QVariantList editorLutOptions READ EditorLutOptions NOTIFY EditorStateChanged)
  Q_PROPERTY(int editorLutIndex READ EditorLutIndex NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorExposure READ EditorExposure NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorContrast READ EditorContrast NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorSaturation READ EditorSaturation NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorTint READ EditorTint NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorBlacks READ EditorBlacks NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorWhites READ EditorWhites NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorShadows READ EditorShadows NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorHighlights READ EditorHighlights NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorSharpen READ EditorSharpen NOTIFY EditorStateChanged)
  Q_PROPERTY(double editorClarity READ EditorClarity NOTIFY EditorStateChanged)

 public:
  EditorController(ProjectModule* project, LibraryModule* library, QObject* parent = nullptr);

  Q_INVOKABLE void OpenEditor(uint elementId, uint imageId);
  Q_INVOKABLE void CloseEditor();
  Q_INVOKABLE void ResetEditorAdjustments();
  Q_INVOKABLE void RequestEditorFullPreview();
  Q_INVOKABLE void SetEditorLutIndex(int index);
  Q_INVOKABLE void SetEditorExposure(double value);
  Q_INVOKABLE void SetEditorContrast(double value);
  Q_INVOKABLE void SetEditorSaturation(double value);
  Q_INVOKABLE void SetEditorTint(double value);
  Q_INVOKABLE void SetEditorBlacks(double value);
  Q_INVOKABLE void SetEditorWhites(double value);
  Q_INVOKABLE void SetEditorShadows(double value);
  Q_INVOKABLE void SetEditorHighlights(double value);
  Q_INVOKABLE void SetEditorSharpen(double value);
  Q_INVOKABLE void SetEditorClarity(double value);

  void InitializeEditorLuts();
  void FinalizeEditorSession(bool persistChanges);

  [[nodiscard]] bool EditorActive() const { return editor_active_; }
  [[nodiscard]] bool editor_active() const { return editor_active_; }
  [[nodiscard]] bool EditorBusy() const { return editor_busy_; }
  [[nodiscard]] bool editor_busy() const { return editor_busy_; }
  [[nodiscard]] auto EditorElementId() const -> uint {
    return static_cast<uint>(editor_element_id_);
  }
  [[nodiscard]] auto editor_element_id() const -> sl_element_id_t { return editor_element_id_; }
  [[nodiscard]] auto editor_image_id() const -> image_id_t { return editor_image_id_; }
  /// Resolve the RAW Decode controls for the image currently owned by the
  /// application project.  UI code receives the result through the session
  /// facade; it does not inspect Image or pipeline state itself.
  [[nodiscard]] auto RawDecodeCapabilitiesForImage(image_id_t image_id) const -> QVariantMap;
  [[nodiscard]] auto EditorTitle() const -> QString { return editor_title_text_.Render(); }
  [[nodiscard]] auto editor_title() const -> QString { return editor_title_text_.Render(); }
  [[nodiscard]] auto EditorStatus() const -> QString { return editor_status_text_.Render(); }
  [[nodiscard]] auto editor_status() const -> QString { return editor_status_text_.Render(); }
  [[nodiscard]] auto EditorPreviewUrl() const -> QString { return editor_preview_url_; }
  [[nodiscard]] auto editor_preview_url() const -> const QString& { return editor_preview_url_; }
  [[nodiscard]] auto EditorLutOptions() const -> QVariantList { return editor_lut_options_; }
  [[nodiscard]] auto editor_lut_options() const -> const QVariantList& {
    return editor_lut_options_;
  }
  [[nodiscard]] int  EditorLutIndex() const { return editor_lut_index_; }
  [[nodiscard]] int  editor_lut_index() const { return editor_lut_index_; }
  [[nodiscard]] auto EditorExposure() const -> double { return editor_state_.exposure_; }
  [[nodiscard]] auto EditorContrast() const -> double { return editor_state_.contrast_; }
  [[nodiscard]] auto EditorSaturation() const -> double { return editor_state_.saturation_; }
  [[nodiscard]] auto EditorTint() const -> double { return editor_state_.tint_; }
  [[nodiscard]] auto EditorBlacks() const -> double { return editor_state_.blacks_; }
  [[nodiscard]] auto EditorWhites() const -> double { return editor_state_.whites_; }
  [[nodiscard]] auto EditorShadows() const -> double { return editor_state_.shadows_; }
  [[nodiscard]] auto EditorHighlights() const -> double { return editor_state_.highlights_; }
  [[nodiscard]] auto EditorSharpen() const -> double { return editor_state_.sharpen_; }
  [[nodiscard]] auto EditorClarity() const -> double { return editor_state_.clarity_; }
  [[nodiscard]] auto editor_state() const -> const EditorState& { return editor_state_; }

 signals:
  void EditorStateChanged();
  void EditorPreviewChanged();

 private:
  int  LutIndexForPath(const std::string& lutPath) const;
  bool LoadEditorStateFromPipeline();
  void SetupEditorPipeline();
  void ApplyEditorStateToPipeline();
  void QueueEditorRender(RenderType renderType);
  void StartNextEditorRender();
  void PollEditorRender();
  void EnsureEditorPollTimer();
  bool UpdateEditorPreviewFromBuffer(const std::shared_ptr<ImageBuffer>& buffer);
  void SetEditorAdjustment(float& field, double value, double minValue, double maxValue);

  ProjectModule* project_ = nullptr;
  LibraryModule* library_ = nullptr;

  bool                    editor_active_     = false;
  bool                    editor_busy_       = false;
  sl_element_id_t         editor_element_id_ = 0;
  image_id_t              editor_image_id_   = 0;
  i18n::LocalizedText     editor_title_text_{};
  i18n::LocalizedText     editor_status_text_{};
  QString                 editor_preview_url_{};
  QVariantList            editor_lut_options_{};
  std::vector<std::string> editor_lut_paths_{};
  int                     editor_lut_index_ = 0;
  EditorState             editor_state_{};
  EditorState             editor_initial_state_{};
  EditorState             editor_pending_state_{};
  RenderType              editor_pending_render_type_ = RenderType::FAST_PREVIEW;
  bool                    editor_has_pending_render_  = false;
  bool                    editor_render_inflight_     = false;
  std::shared_ptr<PipelineGuard>     editor_pipeline_guard_{};
  std::shared_ptr<PipelineScheduler> editor_scheduler_{};
  PipelineTask                       editor_base_task_{};
  QTimer*                            editor_poll_timer_ = nullptr;
  std::optional<std::future<std::shared_ptr<ImageBuffer>>> editor_render_future_{};
};

}  // namespace alcedo::ui
