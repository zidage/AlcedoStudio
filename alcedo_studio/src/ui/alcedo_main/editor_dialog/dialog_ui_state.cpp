#define ALCEDO_EDITOR_DIALOG_INTERNAL
#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"

namespace alcedo::ui {

auto EditorDialog::DefaultAdjustmentState() -> const AdjustmentState& {
  static const AdjustmentState defaults{};
  return defaults;
}

void EditorDialog::UpdateViewerZoomLabel(float zoom) {
  if (!viewer_zoom_value_label_) {
    return;
  }
  const float clamped = std::max(1.0f, zoom);
  viewer_zoom_value_label_->setText(Tr("%1%").arg(clamped * 100.0f, 0, 'f', 0));

  if (viewer_zoom_resolution_label_) {
    const uint32_t w = exif_display_.width_;
    const uint32_t h = exif_display_.height_;
    if (w > 0 && h > 0) {
      viewer_zoom_resolution_label_->setText(Tr("%1 × %2 px").arg(w).arg(h));
    } else {
      viewer_zoom_resolution_label_->setText(QStringLiteral("-- × -- px"));
    }
  }
}

void EditorDialog::RefreshPanelSwitchUi() {
  if (!tone_panel_btn_ || !look_panel_btn_ || !drt_panel_btn_ || !geometry_panel_btn_ ||
      !raw_panel_btn_) {
    return;
  }
  const bool tone_active     = (active_panel_ == ControlPanelKind::Tone);
  const bool look_active     = (active_panel_ == ControlPanelKind::Look);
  const bool drt_active      = (active_panel_ == ControlPanelKind::DisplayRenderingTransform);
  const bool geometry_active = (active_panel_ == ControlPanelKind::Geometry);
  const bool raw_active      = (active_panel_ == ControlPanelKind::RawDecode);

  const auto apply_panel_button_state = [](QPushButton* button, bool active, bool is_first,
                                           bool is_last) {
    if (!button) {
      return;
    }

    button->setChecked(active);
    button->setIcon(RenderPanelToggleIcon(
        button->property(kPanelIconPathProperty).toString(),
        active ? AppTheme::Instance().bgCanvasColor() : AppTheme::Instance().textColor(),
        kPanelToggleIconSize, button->devicePixelRatioF()));
    button->setStyleSheet(AppTheme::EditorPanelToggleStyle(active, is_first, is_last));
  };

  apply_panel_button_state(tone_panel_btn_, tone_active, true, false);
  apply_panel_button_state(look_panel_btn_, look_active, false, false);
  apply_panel_button_state(drt_panel_btn_, drt_active, false, false);
  apply_panel_button_state(geometry_panel_btn_, geometry_active, false, false);
  apply_panel_button_state(raw_panel_btn_, raw_active, false, true);
}

void EditorDialog::SetActiveControlPanel(ControlPanelKind panel) {
  const ControlPanelKind previous_panel = active_panel_;
  if (previous_panel == ControlPanelKind::Geometry && panel != ControlPanelKind::Geometry &&
      geometry_panel_) {
    geometry_panel_->CommitPendingCrop();
  }
  active_panel_ = panel;
  if (control_panels_stack_) {
    int panel_index = 0;
    if (panel == ControlPanelKind::Look) {
      panel_index = 1;
    } else if (panel == ControlPanelKind::DisplayRenderingTransform) {
      panel_index = 2;
    } else if (panel == ControlPanelKind::Geometry) {
      panel_index = 3;
    } else if (panel == ControlPanelKind::RawDecode) {
      panel_index = 4;
    }
    control_panels_stack_->setCurrentIndex(panel_index);
  }

  const bool geometry_active = (panel == ControlPanelKind::Geometry);

  if (viewer_) {
    if (geometry_panel_) {
      if (geometry_active && previous_panel != ControlPanelKind::Geometry) {
        geometry_panel_->BeginCropEditingSession();
      } else {
        geometry_panel_->SyncControlsFromDialogState();
      }
    }
    viewer_->SetCropOverlayVisible(geometry_active);
    viewer_->SetCropToolEnabled(geometry_active);
  }
  RefreshPanelSwitchUi();
  if (pipeline_initialized_) {
    const bool geometry_transition =
        previous_panel == ControlPanelKind::Geometry || panel == ControlPanelKind::Geometry;
    if (geometry_transition) {
      if (render_coordinator_) {
        render_coordinator_->InvalidateDetailPreviewState();
      }
    }
    if (render_coordinator_) {
      render_coordinator_->RequestRender(
          frame_manager_.UseViewportRegionForPanelChange(previous_panel, panel));
      render_coordinator_->ScheduleQualityPreviewRenderFromPipeline();
    }
  }
}

void EditorDialog::SyncControlsFromState() {
  if (!controls_) {
    return;
  }

  syncing_controls_ = true;
  LoadActiveHlsProfile(state_);
  SanitizeOdtStateForUi(state_.odt_);

  if (raw_panel_) {
    raw_panel_->SyncControlsFromDialogState();
  }
  if (look_panel_) {
    look_panel_->SyncControlsFromDialogState();
  }
  if (tone_panel_) {
    tone_panel_->ReloadFromCommittedState();
  }
  if (drt_panel_) {
    drt_panel_->SyncControlsFromDialogState();
  }
  if (geometry_panel_) {
    geometry_panel_->SyncControlsFromDialogState();
  }
  if (viewer_) {
    frame_manager_.SyncViewerDisplayEncoding(state_.odt_.encoding_space_,
                                             state_.odt_.encoding_eotf_,
                                             state_.odt_.peak_luminance_);
    const bool geometry_active = (active_panel_ == ControlPanelKind::Geometry);
    viewer_->SetCropOverlayVisible(geometry_active);
    viewer_->SetCropToolEnabled(geometry_active);
  }
  syncing_controls_ = false;
}
}  // namespace alcedo::ui
