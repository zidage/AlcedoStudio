//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

/**
 * @brief Builds sparse v6 transfer packages from one source document.
 *
 * Validates stable selection identities against the replayed source document,
 * preserves source backbone order, copies only selected values, and copies all
 * Masks or no Masks for each selected Color Grade. Never changes source state.
 */
class AdjustmentTransferPackageBuilder {
 public:
  AdjustmentTransferPackageBuilder() = delete;

  /**
   * @brief Build one validated v6 package.
   *
   * Every selected @ref AdjustmentTransferNodeSelection::node_id must be a
   * Color Grade on the source image backbone or the source DRT/Post endpoint.
   * Every @ref AdjustmentTransferItemKind::Adjustment item must name an
   * @ref AdjustmentInstanceId owned by that node. Package Color Grade order is
   * the source backbone order, not the selection order.
   *
   * @param document Replayed or live source document. Read-only.
   * @param selection Stable node and item identities to transfer.
   * @return Validated package with a computed fingerprint.
   * @throws std::runtime_error on an empty selection, an unknown or duplicated
   *         node, an item the node does not own, a duplicated item, or a
   *         selection that produces no transferable content.
   */
  [[nodiscard]] static auto Build(const PipelineDocument&            document,
                                  const AdjustmentTransferSelection& selection)
      -> AdjustmentTransferPackage;
};

/**
 * @brief Selection containing every transferable item of @p document.
 *
 * Lists each backbone Color Grade with node fields, every adjustment, and a
 * `Masks` item when the Grade owns at least one Mask, then the DRT/Post
 * endpoint with Display Transform and every post adjustment. Order follows the
 * document, so building from this selection captures the complete transferable
 * state. @p source_version_id is left empty; callers attach UI provenance.
 */
[[nodiscard]] auto SelectAllTransferableItems(const PipelineDocument& document)
    -> AdjustmentTransferSelection;

}  // namespace alcedo
