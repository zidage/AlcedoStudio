//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <optional>

#include "ui/alcedo_main/editor_dialog/modules/lens_calib.hpp"
#include "ui/alcedo_main/editor_dialog/session/adjustment_panel.hpp"
#include "ui/alcedo_main/editor_dialog/state.hpp"
#include "ui/alcedo_main/editor_dialog/state/raw_decode_adjustment_state.hpp"

namespace alcedo::ui {

class EditorAdjustmentSession;

class RawDecodePanelWidget final : public AdjustmentPanelWidget {
 public:
  struct Dependencies {
    EditorAdjustmentSession* session                = nullptr;
    QVBoxLayout*             panel_layout           = nullptr;
    AdjustmentState*         dialog_state           = nullptr;
    AdjustmentState*         dialog_committed_state = nullptr;
  };

  struct Callbacks {
    std::function<bool()> is_global_syncing;
    std::function<void()> request_render;
    std::function<std::optional<RawDecodeAdjustmentState>(const RawDecodeAdjustmentState&)>
        load_from_pipeline;
  };

  explicit RawDecodePanelWidget(QWidget* parent = nullptr);

  void Configure(Dependencies deps, Callbacks callbacks);
  void Build();

  auto PanelId() const -> AdjustmentPanelId override { return AdjustmentPanelId::RawDecode; }
  void LoadFromPipeline() override;
  void ReloadFromCommittedState() override;
  void SetSyncing(bool syncing) override;

  void SyncControlsFromDialogState();
  void RetranslateUi();

 private:
  void                     BuildDecodeSection();
  void                     BuildLensSection();
  void                     RefreshDemosaicMethodComboFromState();
  void                     EnsureLensCatalogLoaded();
  void                     RefreshLensBrandComboFromState();
  void                     RefreshLensModelComboFromState();
  void                     RefreshLensComboFromState();
  void                     ProjectRawStateToDialog();
  void                     PullRawStateFromDialog();
  void                     PullCommittedRawStateFromDialog();
  auto                     IsSyncing() const -> bool;
  void                     RequestPipelineRender();
  void                     PreviewRawField(AdjustmentField field);
  void                     CommitRawField(AdjustmentField field);

  Dependencies             deps_{};
  Callbacks                callbacks_{};
  bool                     local_syncing_ = false;

  RawDecodeAdjustmentState raw_state_{};
  RawDecodeAdjustmentState committed_raw_state_{};
  lens_calib::LensCatalog  lens_catalog_{};

  QComboBox*               raw_demosaic_method_combo_           = nullptr;
  QCheckBox*               raw_highlights_reconstruct_checkbox_ = nullptr;
  QCheckBox*               lens_calib_enabled_checkbox_         = nullptr;
  QComboBox*               lens_brand_combo_                    = nullptr;
  QComboBox*               lens_model_combo_                    = nullptr;
  QLabel*                  lens_catalog_status_label_           = nullptr;
};

}  // namespace alcedo::ui
