//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "app/pipeline_service.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline.hpp"
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

  // Runtime-resolved values are image-derived. Keep these false for normal copy/paste.
  bool                                  include_color_temperature_resolved_values_ = false;
  bool                                  include_lens_calibration_runtime_metadata_ = false;

  // Optional UI/SDK fine selection. If set, only listed operators can be captured.
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

// --- Phase 6C-8: Mini-Git Paste and Merge types ---

/// One conflicting field discovered during a merge between the current Version head and an
/// incoming branch. The UI must resolve every conflict before a merge commit can be created.
struct AdjustmentMergeConflict {
  PipelineStageName stage         = PipelineStageName::Stage_Count;
  OperatorType      operator_type = OperatorType::UNKNOWN;
  std::string       field_key;       ///< Stable field identity, e.g. "exposure", "contrast"
  nlohmann::json    current_value;   ///< Value at the current Version head
  nlohmann::json    incoming_value;  ///< Value in the incoming branch
};

/// UI-provided resolution for one conflicting field. The field_key matches the identity in
/// AdjustmentMergeConflict so the service can map resolutions back to specific operators.
struct AdjustmentMergeResolution {
  std::string    field_key;        ///< Matches AdjustmentMergeConflict::field_key
  nlohmann::json resolved_value    = nlohmann::json(nullptr);
  bool           resolved_enabled  = true;
};

/// Result of initiating a merge. Contains the incoming branch identity and any per-field
/// conflicts that the UI must resolve before CompleteMerge can be called.
struct AdjustmentMergePreview {
  bool                                 has_conflicts = false;
  std::vector<AdjustmentMergeConflict> conflicts;
  version_ref_id_t                     incoming_version_id{};  ///< Temporary Version for the incoming branch
  commit_hash_t                        incoming_head{};        ///< Head commit of the incoming branch
  std::string                          error;
};

/// Result of completing a merge with UI-provided resolutions.
struct AdjustmentMergeResult {
  bool              merged = false;
  version_ref_id_t  active_version_id{};  ///< The active Version advanced to the merge commit
  commit_hash_t     merge_commit_hash{};  ///< Hash of the created merge commit
  std::string       error;
};

/// Result of pasting adjustments as a new root-relative Version.
struct AdjustmentPasteResult {
  bool              pasted = false;
  version_ref_id_t  new_version_id{};  ///< The newly created Version ref
  commit_hash_t     new_head{};        ///< Head commit of the pasted branch
  std::string       error;
};

class AdjustmentTransferService final {
 public:
  AdjustmentTransferService() = delete;

  [[nodiscard]] static auto Capture(PipelineExecutor&                  source,
                                    const AdjustmentTransferSelection& selection = {})
      -> AdjustmentTransferPackage;

  // Accepts stable external JSON, for example:
  // {"schema":"alcedo.adjustment_transfer.v1","operators":[{"operator":"exposure",
  // "params":{"exposure":2.0}}]}
  [[nodiscard]] static auto ImportPackage(const nlohmann::json& package_json)
      -> AdjustmentTransferPackage;
  [[nodiscard]] static auto ExportPackage(const AdjustmentTransferPackage& package)
      -> nlohmann::json;

  // Returns true when at least one target operator actually changed.
  static auto Apply(PipelineExecutor& target, const AdjustmentTransferPackage& package) -> bool;

  // Loads, applies, saves, and syncs selected pipelines. The returned applied ids are the ids whose
  // pipelines changed; callers can invalidate thumbnail caches and refresh album rows for them.
  // This low-level overload is intended for direct pipeline tooling; UI/CLI project operations
  // should prefer the versioned overload below so pasted adjustments participate in edit history.
  [[nodiscard]] static auto Apply(PipelineMgmtService&             pipeline_service,
                                  std::span<const sl_element_id_t> target_ids,
                                  const AdjustmentTransferPackage& package)
      -> AdjustmentApplyResult;

  // --- Phase 6C-8: Mini-Git Paste and Merge ---

  [[nodiscard]] static auto BuildRootRelativeCommits(
      const AdjustmentTransferPackage& package,
      const root_id_t& root_id)
      -> std::vector<EditCommit>;

  /// Paste adjustments as a new root-relative Version. The new Version never inherits the
  /// previously active Version's commits. The caller owns the CommitGraph; after a successful
  /// paste the graph contains the new Version ref, the root-relative commit chain, and the
  /// active Version is set to the new Version. The serialized pipeline state is stored on
  /// ImageEditState.
  [[nodiscard]] static auto PasteAsRootRelativeVersion(
      CommitGraph& graph,
      PipelineMgmtService& pipeline_service,
      sl_element_id_t element_id,
      const AdjustmentTransferPackage& package,
      std::string version_display_name)
      -> AdjustmentPasteResult;

  /// Build an incoming root-relative branch and detect per-field conflicts between the current
  /// active Version head and the incoming branch head. The incoming commits are inserted into
  /// the graph and a temporary Version ref is created for the branch.
  /// On return, has_conflicts indicates whether the UI must resolve fields before
  /// CompleteMerge can be called.
  [[nodiscard]] static auto InitiateMerge(
      CommitGraph& graph,
      PipelineMgmtService& pipeline_service,
      sl_element_id_t element_id,
      const AdjustmentTransferPackage& package,
      std::string incoming_version_display_name)
      -> AdjustmentMergePreview;

  /// Complete a merge after the UI has provided resolutions for every conflicting field.
  /// Creates a two-parent merge commit (first = current Version head, second = incoming branch
  /// head) with the resolved field delta and advances the active Version to the merge commit.
  /// The incoming branch Version ref is retained for ancestry; it is not removed.
  [[nodiscard]] static auto CompleteMerge(
      CommitGraph& graph,
      PipelineMgmtService& pipeline_service,
      const AdjustmentMergePreview& preview,
      const std::vector<AdjustmentMergeResolution>& resolutions)
      -> AdjustmentMergeResult;

  /// Discard the incoming branch created by InitiateMerge. No merge commit is created and the
  /// active Version is not moved. The incoming commits remain in the graph as unreachable
  /// objects that will be collected on clean project exit.
  static void CancelMerge(CommitGraph& graph, AdjustmentMergePreview& preview);
};

}  // namespace alcedo
