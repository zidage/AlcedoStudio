//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/operators/op_base.hpp"
#include "app/editor_session_request_ids.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

struct AdjustmentTransferEntry {
  PipelineStageName stage_         = PipelineStageName::Stage_Count;
  OperatorType      operator_type_ = OperatorType::UNKNOWN;
  bool              enabled_       = true;
  bool              merge_params_  = false;
  nlohmann::json    params_        = nlohmann::json::object();
};

struct AdjustmentTransferPackage {
  std::string                          schema_ = "alcedo.adjustment_transfer.v1";
  std::vector<AdjustmentTransferEntry> operators_;

  [[nodiscard]] auto                   Empty() const -> bool { return operators_.empty(); }
};

struct AdjustmentTransferSelection {
  bool                                  include_geometry_                          = true;
  bool                                  include_tone_                              = true;
  bool                                  include_color_                             = true;
  bool                                  include_color_temperature_                 = true;
  bool                                  include_detail_                            = true;
  bool                                  include_output_transform_                  = true;
  bool                                  include_image_loading_                     = false;
  bool                                  include_lens_calibration_                  = false;
  bool                                  include_color_temperature_resolved_values_ = false;
  bool                                  include_lens_calibration_runtime_metadata_ = false;
  std::optional<std::set<OperatorType>> operator_filter_                           = std::nullopt;
};

struct AdjustmentApplyFailure {
  sl_element_id_t file_id_ = 0;
  std::string     message_;
};

struct AdjustmentApplyResult {
  std::vector<sl_element_id_t>        applied_ids_;
  std::vector<sl_element_id_t>        unchanged_ids_;
  std::vector<AdjustmentApplyFailure> failures_;
};

struct AdjustmentMergeConflict {
  PipelineStageName stage            = PipelineStageName::Stage_Count;
  OperatorType      operator_type    = OperatorType::UNKNOWN;
  std::string       field_key;
  nlohmann::json    current_value;
  nlohmann::json    incoming_value;
  bool              current_enabled  = true;
  bool              incoming_enabled = true;
};

struct AdjustmentMergeResolution {
  std::string                         field_key;
  /// Preferred when set. When nullopt, CompleteMerge infers choice by comparing
  /// resolved_value to the conflict's current/incoming values.
  std::optional<OperatorMergeChoice>  choice;
  nlohmann::json                      resolved_value   = nlohmann::json(nullptr);
  bool                                resolved_enabled = true;
};

struct AdjustmentMergePreview {
  /// Queue-owned identity of this preview. A completion must use the active
  /// preview represented by this id; it is never regenerated from UI fields.
  MergePreviewId                       preview_id{};
  /// Fingerprint of the copied package used to build this preview.
  std::string                          source_package_fingerprint;
  /// First-parent working head observed while the preview was built.
  head_commit_hash_t                   first_parent_head{};
  bool                                 has_conflicts = false;
  std::vector<AdjustmentMergeConflict> conflicts;
  version_ref_id_t                     incoming_version_id{};
  commit_hash_t                        incoming_head{};
  std::string                          error;
};

struct AdjustmentMergeResult {
  bool             merged = false;
  version_ref_id_t active_version_id{};
  commit_hash_t    merge_commit_hash{};
  std::string      error;
};

struct AdjustmentPasteResult {
  bool             pasted = false;
  version_ref_id_t new_version_id{};
  /// Active Version observed before the paste Version was created. Cancel paste
  /// restores this Version and rebuilds the live pipeline to its head.
  version_ref_id_t prior_version_id{};
  commit_hash_t    new_head{};
  std::string      error;
};

}  // namespace alcedo
