#define ALCEDO_EDITOR_DIALOG_INTERNAL
#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"

#include "ui/alcedo_main/editor_dialog/pipeline/display_transform_pipeline_adapter.hpp"
#include "ui/alcedo_main/editor_dialog/pipeline/look_pipeline_adapter.hpp"
#include "ui/alcedo_main/editor_dialog/pipeline/raw_pipeline_adapter.hpp"

namespace alcedo::ui {
namespace {

auto LoadScopeExifDisplayMetaData(const std::shared_ptr<ImagePoolService>& image_pool,
                                  image_id_t image_id) -> ExifDisplayMetaData {
  if (!image_pool || image_id == 0) {
    return {};
  }

  try {
    return image_pool->Read<ExifDisplayMetaData>(
        image_id, [](const std::shared_ptr<Image>& image) -> ExifDisplayMetaData {
          if (!image) {
            return {};
          }
          if (image->has_exif_display_.load()) {
            return image->exif_display_;
          }
          if (image->has_exif_json_.load()) {
            ExifDisplayMetaData metadata;
            metadata.FromJson(image->exif_json_);
            return metadata;
          }
          return {};
        });
  } catch (...) {
    return {};
  }
}

}  // namespace

EditorDialog::EditorDialog(std::shared_ptr<ImagePoolService>       image_pool,
                           std::shared_ptr<PipelineGuard>          pipeline_guard,
                           std::shared_ptr<EditHistoryMgmtService> history_service,
                           std::shared_ptr<EditHistoryGuard>       history_guard,
                           sl_element_id_t element_id, image_id_t image_id, QWidget* parent)
    : QDialog(parent),
      image_pool_(std::move(image_pool)),
      pipeline_guard_(std::move(pipeline_guard)),
      history_service_(std::move(history_service)),
      history_guard_(std::move(history_guard)),
      element_id_(element_id),
      image_id_(image_id),
      scheduler_(RenderService::GetPreviewScheduler()) {
  if (!image_pool_ || !pipeline_guard_ || !pipeline_guard_->pipeline_ || !history_service_ ||
      !history_guard_ || !history_guard_->history_ || !scheduler_) {
    throw std::runtime_error("EditorDialog: missing services");
  }
  history_coordinator_ = std::make_unique<EditorHistoryCoordinator>(
      EditorHistoryCoordinator::Dependencies{
          .history_service = history_service_,
          .history_guard   = history_guard_,
          .pipeline_guard  = pipeline_guard_,
          .element_id      = element_id_,
          .message_parent  = this,
      },
      EditorHistoryCoordinator::Callbacks{
          .reload_ui_state_from_pipeline =
              [this](bool reset_to_defaults_if_missing) {
                const bool loaded = LoadStateFromPipelineIfPresent();
                if (!loaded && !reset_to_defaults_if_missing) {
                  return false;
                }
                if (!loaded) {
                  state_ = AdjustmentState{};
                  SanitizeOdtStateForUi(state_.odt_);
                  UpdateAllCdlWheelDerivedColors(state_);
                  if (tone_panel_) {
                    tone_panel_->ClearSubmittedColorTempRequest();
                  }
                } else {
                  if (tone_panel_) {
                    tone_panel_->MarkSubmittedColorTempRequest(state_);
                  }
                }
                committed_state_ = state_;
                SyncControlsFromState();
                if (render_coordinator_) {
                  render_coordinator_->AdvancePreviewGeneration();
                  render_coordinator_->RequestRenderWithoutApplyingState(
                      /*use_viewport_region=*/false,
                      /*bump_preview_generation=*/false);
                  render_coordinator_->TriggerQualityPreviewRenderFromPipeline();
                }
                return true;
              },
          .after_pipeline_params_imported =
              [this]() {
                frame_manager_.AttachExecutionStages(pipeline_guard_->pipeline_);
                if (look_panel_) {
                  look_panel_->ClearAppliedLutPath();
                }
              },
          .refresh_version_log_selection_styles =
              [this]() {
                if (versioning_panel_) {
                  versioning_panel_->RefreshVersionLogSelectionStyles();
                }
              },
      });
  render_coordinator_ = std::make_unique<EditorRenderCoordinator>(
      EditorRenderCoordinator::Dependencies{
          .timer_parent   = this,
          .pipeline_guard = pipeline_guard_,
          .scheduler      = scheduler_,
          .base_task      = &base_task_,
          .state          = &state_,
      },
      EditorRenderCoordinator::Callbacks{
          .viewer       = [this]() { return viewer_; },
          .spinner      = [this]() { return spinner_; },
          .active_panel = [this]() { return active_panel_; },
          .needs_full_frame_preview_after_geometry_commit =
              [this]() { return frame_manager_.NeedsFullFramePreviewAfterGeometryCommit(); },
          .clear_full_frame_preview_after_geometry_commit =
              [this]() { frame_manager_.ClearNeedsFullFramePreviewAfterGeometryCommit(); },
          .apply_state_to_pipeline =
              [this](const AdjustmentState& render_state) { ApplyStateToPipeline(render_state); },
          .refresh_color_temp_runtime_state =
              [this]() {
                return tone_panel_ && pipeline_guard_ && pipeline_guard_->pipeline_
                           ? tone_panel_->RefreshColorTempRuntimeStateFromGlobalParams(
                                 pipeline_guard_->pipeline_.get())
                           : false;
              },
          .sync_color_temp_controls =
              [this]() {
                if (tone_panel_) {
                  tone_panel_->SyncColorTempControlsFromDialogState();
                }
              },
      });
  adjustment_session_ = std::make_unique<EditorAdjustmentSession>(
      EditorAdjustmentSession::Dependencies{
          .pipeline_guard = pipeline_guard_,
          .working_version =
              history_coordinator_ ? &history_coordinator_->WorkingVersion() : nullptr,
          .state           = &state_,
          .committed_state = &committed_state_,
      },
      EditorAdjustmentSession::Callbacks{
          .schedule_quality_preview =
              [this]() {
                if (render_coordinator_) {
                  render_coordinator_->RequestQualityPreviewRenderFromPipeline(
                      /*apply_state=*/true);
                }
              },
          .schedule_detail_preview_from_viewport =
              [this]() {
                if (render_coordinator_) {
                  render_coordinator_->MaybeScheduleDetailPreviewRenderFromViewport();
                }
              },
          .advance_preview_generation =
              [this]() {
                if (render_coordinator_) {
                  render_coordinator_->AdvancePreviewGeneration();
                }
              },
          .update_version_ui =
              [this]() {
                if (history_coordinator_) {
                  history_coordinator_->UpdateVersionUi();
                }
              },
          .mark_full_frame_preview_after_geometry_commit =
              [this]() { frame_manager_.MarkNeedsFullFramePreviewAfterGeometryCommit(); },
      });

  setModal(true);
  setSizeGripEnabled(true);
  setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
  setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, false);
  setWindowTitle(ResolveEditorWindowTitle(image_pool_, image_id_, element_id_));
  setMinimumSize(1080, 680);
  resize(1500, 1000);
  AppTheme::ApplyFont(this, AppTheme::FontRole::UiBody);

  // --- EditorDialog UI construction. ---
  BuildViewerAndPanelShell();
  if (scope_panel_) {
    exif_display_ = LoadScopeExifDisplayMetaData(image_pool_, image_id_);
    scope_panel_->SetExifDisplayMetaData(exif_display_);
    UpdateViewerZoomLabel(viewer_ ? viewer_->GetViewZoom() : 1.0f);
  }
  BuildToneControlPanel();
  BuildLookPanel();
  BuildDisplayTransformPanel();
  BuildGeometryPanel();
  BuildRawDecodePanel();
  if (versioning_panel_) {
    VersioningPanelWidget::Callbacks versioning_callbacks{
        .undo_last_transaction =
            [this]() {
              if (history_coordinator_) {
                history_coordinator_->UndoLastTransaction();
              }
            },
        .move_history_cursor =
            [this](size_t target_cursor) {
              if (history_coordinator_) {
                history_coordinator_->MoveCursorTo(target_cursor);
              }
            },
        .create_version =
            [this]() {
              if (history_coordinator_) {
                history_coordinator_->CreateVersion();
              }
            },
        .checkout_version_by_id =
            [this](const QString& version_id) {
              if (history_coordinator_) {
                history_coordinator_->CheckoutVersionById(version_id);
              }
            },
        .viewer_geometry = [this]() -> QRect {
          return viewer_container_ ? viewer_container_->geometry() : QRect{};
        },
    };
    versioning_panel_->Configure(this, std::move(versioning_callbacks));
    versioning_panel_->Build();
    spinner_ = new SpinnerWidget(versioning_panel_);
    versioning_panel_->SetBottomStatusWidget(spinner_);
  }
  if (history_coordinator_ && versioning_panel_) {
    history_coordinator_->SetUiContext(versioning_panel_->MakeUiContext());
  }

  shortcut_registry_ = std::make_unique<ShortcutRegistry>(this);
  RegisterShortcuts();

  AppTheme::ApplyFontsRecursively(this);

  if (history_coordinator_) {
    history_coordinator_->UpdateVersionUi();
  }

  frame_manager_.SetViewer(viewer_);
  frame_manager_.SetScopePanel(scope_panel_);
  if (scope_panel_) {
    scope_panel_->SetNeedsRenderCallback([this]() {
      if (render_coordinator_) {
        render_coordinator_->RequestRender(/*use_viewport_region=*/true,
                                           /*bump_preview_generation=*/false);
      }
    });
  }
  SetupPipeline();
  pipeline_initialized_ = true;
  if (viewer_) {
    viewer_->SetCropOverlayAspectLock(false, 1.0f);
    viewer_->SetCropOverlayRectNormalized(state_.crop_x_, state_.crop_y_, state_.crop_w_,
                                          state_.crop_h_);
    viewer_->SetCropOverlayRotationDegrees(state_.rotate_degrees_);
    viewer_->SetCropOverlayVisible(false);
    viewer_->SetCropToolEnabled(false);
  }
  if (viewer_ && geometry_panel_) {
    QObject::connect(viewer_, &QtEditViewer::CropOverlayRectChanged, geometry_panel_,
                     [this](float x, float y, float w, float h, bool /*is_final*/) {
                       if (geometry_panel_) {
                         geometry_panel_->SetCropRectFromViewer(x, y, w, h);
                       }
                     });
    QObject::connect(viewer_, &QtEditViewer::CropOverlayRotationChanged, geometry_panel_,
                     [this](float angle_degrees, bool /*is_final*/) {
                       if (geometry_panel_) {
                         geometry_panel_->SetRotationFromViewer(angle_degrees);
                       }
                     });
  }
  if (viewer_) {
    QObject::connect(viewer_, &QtEditViewer::ViewInteractionSettled, this, [this]() {
      if (render_coordinator_) {
        render_coordinator_->MaybeScheduleDetailPreviewRenderFromViewport();
      }
    });
  }

  // Load a 4K quality base preview first; scheduler transitions back to fast-preview baseline.
  QTimer::singleShot(0, this, [this]() {
    if (render_coordinator_) {
      render_coordinator_->AdvancePreviewGeneration();
      render_coordinator_->TriggerQualityPreviewRenderFromPipeline();
    }
  });
}

void EditorDialog::RegisterShortcuts() {
  if (!shortcut_registry_) {
    return;
  }

  shortcut_registry_->Register({
      .id               = kShortcutUndoHistoryId,
      .description      = Tr("Undo last transaction"),
      .default_sequence = QKeySequence(QKeySequence::Undo),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger =
          [this]() {
            if (!ShouldConsumeUndoShortcutLocally()) {
              if (history_coordinator_) {
                history_coordinator_->UndoLastTransaction();
              }
            }
          },
  });
  shortcut_registry_->Register({
      .id               = kShortcutResetGeometryId,
      .description      = Tr("Reset crop & rotation"),
      .default_sequence = QKeySequence(Qt::CTRL | Qt::Key_R),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger       = [this]() { ResetCropAndRotation(); },
  });
  const auto commit_geometry = [this]() {
    if (active_panel_ != ControlPanelKind::Geometry || !geometry_panel_) {
      return;
    }
    geometry_panel_->CommitPendingCrop();
    SetActiveControlPanel(ControlPanelKind::Tone);
  };
  shortcut_registry_->Register({
      .id               = kShortcutCommitGeometryId,
      .description      = Tr("Apply crop"),
      .default_sequence = QKeySequence(Qt::Key_Return),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger       = commit_geometry,
  });
  shortcut_registry_->Register({
      .id               = kShortcutCommitGeometryNumpadId,
      .description      = Tr("Apply crop"),
      .default_sequence = QKeySequence(Qt::Key_Enter),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger       = commit_geometry,
  });
  shortcut_registry_->Register({
      .id               = kShortcutSelectPrevLutId,
      .description      = Tr("Select previous LUT"),
      .default_sequence = QKeySequence(Qt::Key_Up),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger =
          [this]() {
            if (ShouldConsumeLutNavigationShortcut()) {
              return;
            }
            if (look_panel_) {
              look_panel_->SelectRelativeLut(-1);
            }
          },
  });
  shortcut_registry_->Register({
      .id               = kShortcutSelectNextLutId,
      .description      = Tr("Select next LUT"),
      .default_sequence = QKeySequence(Qt::Key_Down),
      .context          = Qt::WidgetWithChildrenShortcut,
      .on_trigger =
          [this]() {
            if (ShouldConsumeLutNavigationShortcut()) {
              return;
            }
            if (look_panel_) {
              look_panel_->SelectRelativeLut(1);
            }
          },
  });

  if (versioning_panel_ && versioning_panel_->UndoButton()) {
    versioning_panel_->UndoButton()->setToolTip(
        shortcut_registry_->DecorateTooltip(Tr("Undo last transaction"), kShortcutUndoHistoryId));
  }
  if (geometry_panel_ && geometry_panel_->ResetButton()) {
    geometry_panel_->ResetButton()->setToolTip(
        shortcut_registry_->DecorateTooltip(Tr("Reset crop & rotation"), kShortcutResetGeometryId));
  }
}

auto EditorDialog::ShouldConsumeUndoShortcutLocally() const -> bool {
  QWidget* const focus_widget = QApplication::focusWidget();
  if (!focus_widget || !isAncestorOf(focus_widget)) {
    return false;
  }

  return qobject_cast<QLineEdit*>(focus_widget) != nullptr ||
         qobject_cast<QTextEdit*>(focus_widget) != nullptr ||
         qobject_cast<QPlainTextEdit*>(focus_widget) != nullptr ||
         qobject_cast<QAbstractSpinBox*>(focus_widget) != nullptr;
}

auto EditorDialog::ShouldConsumeLutNavigationShortcut() const -> bool {
  if (active_panel_ != ControlPanelKind::Look || !look_panel_) {
    return true;
  }

  QWidget* const focus_widget = QApplication::focusWidget();
  return !look_panel_->CanHandleLutNavigationShortcut(focus_widget);
}

void EditorDialog::changeEvent(QEvent* event) {
  if (event && event->type() == QEvent::LanguageChange) {
    RetranslateUi();
  }
  QDialog::changeEvent(event);
}

void EditorDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  if (initial_splitter_sizes_applied_) {
    return;
  }
  initial_splitter_sizes_applied_ = true;
  QTimer::singleShot(50, this, [this]() { ApplyInitialSplitterSizes(); });
}

void EditorDialog::ApplyInitialSplitterSizes() {
  if (!main_splitter_) {
    return;
  }

  QWidget* controls_panel = main_splitter_->widget(2);
  if (!controls_panel) {
    return;
  }

  const int handle_width = main_splitter_->handleWidth();
  const int available_width =
      std::max(0, main_splitter_->width() - handle_width * (main_splitter_->count() - 1));
  if (available_width <= 0) {
    return;
  }

  const int right_width =
      std::clamp(static_cast<int>(std::lround(static_cast<double>(available_width) * 0.25)),
                 controls_panel->minimumWidth(), controls_panel->maximumWidth());
  const int center_width =
      std::max(400, available_width - VersioningPanelWidget::kCollapsedWidth - right_width);

  main_splitter_->setSizes({VersioningPanelWidget::kCollapsedWidth, center_width, right_width});
}

void EditorDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);
  if (versioning_panel_) {
    versioning_panel_->OnDialogResized();
  }
}

void EditorDialog::RetranslateUi() {
  setWindowTitle(ResolveEditorWindowTitle(image_pool_, image_id_, element_id_));
  RetranslateMarkedObjects(this);

  if (tone_panel_btn_) {
    tone_panel_btn_->setText(Tr("Tone"));
  }
  if (look_panel_btn_) {
    look_panel_btn_->setText(Tr("Color"));
  }
  if (drt_panel_btn_) {
    drt_panel_btn_->setText(Tr("Display RT"));
    drt_panel_btn_->setToolTip(Tr("Display Rendering Transform"));
  }
  if (geometry_panel_btn_) {
    geometry_panel_btn_->setText(Tr("Geometry"));
  }
  if (raw_panel_btn_) {
    raw_panel_btn_->setText(Tr("RAW Decode"));
  }
  if (geometry_panel_) {
    geometry_panel_->RetranslateUi();
  }
  if (drt_panel_) {
    drt_panel_->RetranslateUi();
  }
  if (tone_panel_) {
    if (auto* unsupported_label = tone_panel_->ColorTempUnsupportedLabel()) {
      unsupported_label->setText(Tr("Color temperature/tint is unavailable for this image."));
    }
    tone_panel_->RetranslateColorTempModeCombo();
  }
  if (versioning_panel_) {
    versioning_panel_->RetranslateUi();
  }
  if (look_panel_) {
    look_panel_->RetranslateUi();
  }

  if (raw_panel_) {
    raw_panel_->RetranslateUi();
  }
  UpdateViewerZoomLabel(viewer_ ? viewer_->GetViewZoom() : 1.0f);
  if (history_coordinator_) {
    history_coordinator_->UpdateVersionUi();
  }
}
void EditorDialog::BuildToneControlPanel() {
  if (!tone_panel_ || !controls_layout_) {
    return;
  }

  const auto default_lut_path = look_panel_ ? look_panel_->DefaultLutPath() : std::string{};

  // Seed a working version from the latest committed one (if any) before reading panel state.
  if (history_coordinator_) {
    history_coordinator_->SeedWorkingVersionFromActive();
  }

  // If the pipeline already has operator params (loaded from PipelineService/storage or history),
  // initialize UI state from those params rather than overwriting them.
  const bool loaded_state_from_pipeline = LoadStateFromPipelineIfPresent();
  if (!loaded_state_from_pipeline) {
    // Demo-friendly default: apply a LUT only for brand-new pipelines with no saved params.
    state_.lut_path_ = default_lut_path;
    UpdateAllCdlWheelDerivedColors(state_);
  }
  committed_state_ = state_;
  ToneControlPanelWidget::Dependencies deps{
      .session                = adjustment_session_.get(),
      .panel_layout           = controls_layout_,
      .dialog_state           = &state_,
      .dialog_committed_state = &committed_state_,
  };

  ToneControlPanelWidget::Callbacks callbacks{
      .is_global_syncing = [this]() { return syncing_controls_; },
      .request_render =
          [this]() {
            if (render_coordinator_) {
              render_coordinator_->RequestRender();
            }
          },
      .default_adjustment_state = [this]() -> const AdjustmentState& {
        return DefaultAdjustmentState();
      },
      .sync_controls_from_state = [this]() { SyncControlsFromState(); },
  };

  tone_panel_->Configure(std::move(deps), std::move(callbacks));
  tone_panel_->Build();
}

void EditorDialog::BuildLookPanel() {
  if (!look_panel_) {
    return;
  }

  LookControlPanelWidget::Dependencies deps{
      .session                = adjustment_session_.get(),
      .dialog_state           = &state_,
      .dialog_committed_state = &committed_state_,
  };

  LookControlPanelWidget::Callbacks callbacks{
      .is_global_syncing = [this]() { return syncing_controls_; },
      .request_render =
          [this]() {
            if (render_coordinator_) {
              render_coordinator_->RequestRender();
            }
          },
      .default_adjustment_state = [this]() -> const AdjustmentState& {
        return DefaultAdjustmentState();
      },
      .load_from_pipeline =
          [this](const LookAdjustmentState& base) -> std::optional<LookAdjustmentState> {
        if (!pipeline_guard_ || !pipeline_guard_->pipeline_) {
          return std::nullopt;
        }
        const auto loaded = LookPipelineAdapter::Load(*pipeline_guard_->pipeline_, base);
        if (!loaded.loaded_any) {
          return std::nullopt;
        }
        return loaded.state;
      },
  };

  look_panel_->Configure(std::move(deps), std::move(callbacks));
  look_panel_->Build();
}

void EditorDialog::BuildDisplayTransformPanel() {
  if (!drt_panel_ || !drt_controls_layout_) {
    return;
  }

  DisplayTransformPanelWidget::Dependencies deps{
      .session                = adjustment_session_.get(),
      .panel_layout           = drt_controls_layout_,
      .dialog_state           = &state_,
      .dialog_committed_state = &committed_state_,
  };

  DisplayTransformPanelWidget::Callbacks callbacks{
      .is_global_syncing = [this]() { return syncing_controls_; },
      .request_render =
          [this]() {
            if (render_coordinator_) {
              render_coordinator_->RequestRender();
            }
          },
      .default_adjustment_state = [this]() -> const AdjustmentState& {
        return DefaultAdjustmentState();
      },
      .sync_display_encoding =
          [this](ColorUtils::ColorSpace encoding_space, ColorUtils::EOTF encoding_eotf,
                 float peak_luminance) {
            frame_manager_.SyncViewerDisplayEncoding(encoding_space, encoding_eotf,
                                                     peak_luminance);
          },
      .load_from_pipeline = [this](const DisplayTransformAdjustmentState& base)
          -> std::optional<DisplayTransformAdjustmentState> {
        if (!pipeline_guard_ || !pipeline_guard_->pipeline_) {
          return std::nullopt;
        }
        const auto loaded =
            DisplayTransformPipelineAdapter::Load(*pipeline_guard_->pipeline_, base);
        if (!loaded.loaded_any) {
          return std::nullopt;
        }
        return loaded.state;
      },
  };

  drt_panel_->Configure(std::move(deps), std::move(callbacks));
  drt_panel_->Build();
}

void EditorDialog::BuildGeometryPanel() {
  if (!geometry_panel_ || !geometry_controls_layout_) {
    return;
  }

  GeometryPanelWidget::Dependencies deps{
      .session                = adjustment_session_.get(),
      .panel_layout           = geometry_controls_layout_,
      .dialog_state           = &state_,
      .dialog_committed_state = &committed_state_,
  };

  GeometryPanelWidget::Callbacks callbacks{
      .is_global_syncing = [this]() { return syncing_controls_; },
      .request_render =
          [this]() {
            if (render_coordinator_) {
              render_coordinator_->RequestRender();
            }
          },
      .set_crop_overlay_aspect_lock =
          [this](bool locked, float ratio) {
            if (viewer_) {
              viewer_->SetCropOverlayAspectLock(locked, ratio);
            }
          },
      .set_crop_overlay_rect =
          [this](float x, float y, float w, float h) {
            if (viewer_) {
              viewer_->SetCropOverlayRectNormalized(x, y, w, h);
            }
          },
      .set_crop_overlay_rotation =
          [this](float degrees) {
            if (viewer_) {
              viewer_->SetCropOverlayRotationDegrees(degrees);
            }
          },
      .set_crop_overlay_visible =
          [this](bool visible) {
            if (viewer_) {
              viewer_->SetCropOverlayVisible(visible);
            }
          },
      .set_crop_tool_enabled =
          [this](bool enabled) {
            if (viewer_) {
              viewer_->SetCropToolEnabled(enabled);
            }
          },
      .source_aspect_ratio = [this]() -> float {
        if (viewer_ && viewer_->GetWidth() > 0 && viewer_->GetHeight() > 0) {
          return static_cast<float>(viewer_->GetWidth()) / static_cast<float>(viewer_->GetHeight());
        }
        return 1.0f;
      },
  };

  geometry_panel_->Configure(std::move(deps), std::move(callbacks));
  geometry_panel_->Build();
}

void EditorDialog::BuildRawDecodePanel() {
  if (!raw_panel_ || !raw_controls_layout_) {
    return;
  }

  RawDecodePanelWidget::Dependencies deps{
      .session                = adjustment_session_.get(),
      .panel_layout           = raw_controls_layout_,
      .dialog_state           = &state_,
      .dialog_committed_state = &committed_state_,
  };

  RawDecodePanelWidget::Callbacks callbacks{
      .is_global_syncing = [this]() { return syncing_controls_; },
      .request_render =
          [this]() {
            if (render_coordinator_) {
              render_coordinator_->RequestRender(/*use_viewport_region=*/true,
                                                 /*bump_preview_generation=*/false);
            }
          },
      .load_from_pipeline =
          [this](const RawDecodeAdjustmentState& base) -> std::optional<RawDecodeAdjustmentState> {
        if (!pipeline_guard_ || !pipeline_guard_->pipeline_) {
          return std::nullopt;
        }
        const auto loaded = RawPipelineAdapter::Load(*pipeline_guard_->pipeline_, base);
        if (!loaded.loaded_any) {
          return std::nullopt;
        }
        return loaded.state;
      },
  };

  raw_panel_->Configure(std::move(deps), std::move(callbacks));
  raw_panel_->Build();
}

auto RunEditorDialog(std::shared_ptr<ImagePoolService>       image_pool,
                     std::shared_ptr<PipelineGuard>          pipeline_guard,
                     std::shared_ptr<EditHistoryMgmtService> history_service,
                     std::shared_ptr<EditHistoryGuard> history_guard, sl_element_id_t element_id,
                     image_id_t image_id, QWidget* parent) -> bool {
  EditorDialog dlg(std::move(image_pool), std::move(pipeline_guard), std::move(history_service),
                   std::move(history_guard), element_id, image_id, parent);
  dlg.showMaximized();
  dlg.exec();
  return true;
}

}  // namespace alcedo::ui
