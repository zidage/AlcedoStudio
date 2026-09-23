//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Transferable item kinds on one node. @ref Adjustment requires
/// @ref AdjustmentTransferItemSelection::adjustment_id; the other kinds describe
/// one complete node value each.
enum class AdjustmentTransferItemKind : std::uint8_t {
  NodeEnabled,
  NodeMix,
  Adjustment,
  /// All-or-none Mask set of one Color Grade. Never one item per Mask.
  Masks,
  /// Complete DRT parameter value of the DRT/Post endpoint. Indivisible.
  DrtParameters,
};

/**
 * @brief One selected transferable item on one node.
 *
 * Carries stable domain identity only: no row index and no display string.
 */
struct AdjustmentTransferItemSelection {
  AdjustmentTransferItemKind          kind = AdjustmentTransferItemKind::Adjustment;
  /// Required when @p kind is @ref AdjustmentTransferItemKind::Adjustment;
  /// must be empty for every other kind.
  std::optional<AdjustmentInstanceId> adjustment_id;
};

/**
 * @brief Selected items of one node. @p node_id is a Color Grade on the source
 *        image backbone or the source DRT/Post endpoint.
 */
struct AdjustmentTransferNodeSelection {
  NodeId                                       node_id;
  std::vector<AdjustmentTransferItemSelection> items;
};

/**
 * @brief Stable Copy selection: one source Version plus per-node item choices.
 *
 * @p source_version_id is UI provenance. The package does not copy it to the
 * target image.
 */
struct AdjustmentTransferSelection {
  version_ref_id_t                             source_version_id;
  std::vector<AdjustmentTransferNodeSelection> nodes;
};

/// One selected adjustment value inside a sparse transfer entry.
struct TransferAdjustmentValue {
  AdjustmentInstanceId source_id;
  OperatorTypeId       type;
  /// Complete parameter payload of the source adjustment Model.
  nlohmann::json       params = nlohmann::json::object();
};

/**
 * @brief Sparse v6 Color Grade transfer entry.
 *
 * @p enabled, @p mix, and @p masks are absent unless the matching item was
 * selected. An absent @p masks transfers no Mask; a present set transfers every
 * source Mask in source order. @p display_name and @p deletion_protected are
 * structural document data and always travel with an included Grade.
 */
struct TransferColorGradeValue {
  NodeId                                source_node_id;
  std::string                           display_name;
  bool                                  deletion_protected = false;
  std::optional<bool>                   enabled;
  std::optional<float>                  mix;
  std::vector<TransferAdjustmentValue>  adjustments;
  std::optional<std::vector<MaskModel>> masks;
};

/**
 * @brief Sparse v6 DRT/Post transfer entry.
 *
 * @p params holds the complete DRT parameter value when Display Transform was
 * selected. Each entry in @p adjustments is one selected post adjustment.
 * Does not store target node identity.
 */
struct TransferDrtPostValue {
  std::optional<nlohmann::json>        params;
  std::vector<TransferAdjustmentValue> adjustments;

  [[nodiscard]] auto Empty() const -> bool {
    return !params.has_value() && adjustments.empty();
  }
};

/**
 * @brief Portable sparse Color Grade, Mask, and DRT/Post selection for Paste.
 *
 * Contains only selected values. Does not contain Develop, RAW metadata,
 * geometry, history, Version ids, cache paths, or UI state. @p fingerprint_ is
 * a hash of the canonical JSON without that field.
 */
struct AdjustmentTransferPackage {
  std::string                          schema_ = std::string{kAdjustmentTransferSchema};
  std::uint32_t                        document_format_version_ = kPipelineDocumentFormatVersion;
  /// Included Color Grades in source backbone order.
  std::vector<TransferColorGradeValue> color_grades_;
  /// Source default Grade when it is part of the selection; empty otherwise.
  NodeId                               default_grade_id_;
  TransferDrtPostValue                 drt_post_;
  std::string                          fingerprint_;

  /// True when nothing transferable was selected anywhere in the package.
  [[nodiscard]] auto Empty() const -> bool {
    return color_grades_.empty() && drt_post_.Empty();
  }
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

struct AdjustmentPasteResult {
  bool             pasted = false;
  version_ref_id_t new_version_id{};
  /// Active Version observed before the paste Version was created.
  version_ref_id_t prior_version_id{};
  commit_hash_t    new_head{};
  std::string      error;
};

}  // namespace alcedo
