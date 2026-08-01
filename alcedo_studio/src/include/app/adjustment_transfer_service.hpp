//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_adjustment_types.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

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
  /// Stable fingerprint of the canonical package document used to reject a
  /// merge completion that no longer belongs to its preview.
  [[nodiscard]] static auto PackageFingerprint(const AdjustmentTransferPackage& package)
      -> std::string;

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

  [[nodiscard]] static auto BuildRootRelativeCommits(const AdjustmentTransferPackage& package,
                                                     const root_id_t&                 root_id)
      -> std::vector<EditCommit>;

  /// Paste adjustments as a new root-relative Version. The new Version never inherits the
  /// previously active Version's commits. The caller owns the CommitGraph; after a successful
  /// paste the graph contains the new Version ref, the root-relative commit chain, and the
  /// active Version is set to the new Version. The serialized pipeline state is stored on
  /// ImageEditState.
  [[nodiscard]] static auto PasteAsRootRelativeVersion(CommitGraph&         graph,
                                                       const AdjustmentTransferPackage& package,
                                                       std::string version_display_name)
      -> AdjustmentPasteResult;

  [[nodiscard]] static auto PasteAsRootRelativeVersion(CommitGraph&         graph,
                                                       PipelineMgmtService& pipeline_service,
                                                       sl_element_id_t      element_id,
                                                       const AdjustmentTransferPackage& package,
                                                       std::string version_display_name)
      -> AdjustmentPasteResult;

  // Editor merge lives on the session live path only:
  // `EditorHistoryTransfer::BeginLiveMerge` / `CompleteLiveMerge`. Do not reintroduce a
  // service-level InitiateMerge that stages temporary Version refs or shadow graphs.
};

}  // namespace alcedo
