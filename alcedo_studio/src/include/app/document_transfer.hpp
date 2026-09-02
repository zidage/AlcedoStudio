//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Supplies new node, adjustment, and Mask identities during Paste remap.
 *
 * Implementations must return non-empty IDs. The paste planner rejects a value
 * that collides with a source ID, a target ID, or an earlier generated ID.
 * Callers inject a deterministic source in tests. Not thread-safe unless the
 * implementation documents otherwise.
 */
class TransferIdentitySource {
 public:
  virtual ~TransferIdentitySource() = default;

  /** @brief Next Color Grade NodeId. Must not be empty. */
  virtual auto NextNodeId() -> NodeId = 0;

  /**
   * @brief Next adjustment instance id for @p node_id and catalog @p type.
   *
   * Default factories use `{node_id}.{suffix}`. Duplicate types on one node must
   * still yield distinct values.
   */
  virtual auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId = 0;

  /** @brief Next MaskId. Must not be empty. */
  virtual auto NextMaskId() -> MaskId = 0;
};

/**
 * @brief Sequential identity source for deterministic Paste tests.
 *
 * Node ids are `grade.tN` and Mask ids are `mask.tN` with N starting at 1.
 * Adjustment ids use @ref MakeAdjustmentInstanceId.
 */
class CountingTransferIdentitySource final : public TransferIdentitySource {
 public:
  auto NextNodeId() -> NodeId override;
  auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId override;
  auto NextMaskId() -> MaskId override;

 private:
  std::uint32_t next_node_ = 1;
  std::uint32_t next_mask_ = 1;
};

/**
 * @brief Prepared Paste: remapped package, typed batch, and copied asset keys.
 *
 * @p batch is validated. It is not applied to a live document. Asset copies into
 * the target store finish before this value is returned. An unreferenced equal
 * asset is harmless.
 */
struct PreparedDocumentPaste {
  AdjustmentTransferPackage package;
  PipelineEditBatch         batch;
};

/**
 * @brief Optional Paste collaborators. Null identity uses a per-call default source
 *        or the testing hook. Null Mask stores are valid when the package has no
 *        Brush keys.
 */
struct DocumentTransferPasteOptions {
  TransferIdentitySource* identity_source   = nullptr;
  MaskStore*              source_mask_store = nullptr;
  MaskStore*              target_mask_store = nullptr;
};

/**
 * @brief Capture transferable Color Grades, Masks, and DRT/Post from @p document.
 *
 * Omits Develop, RAW metadata, geometry, history, Version ids, and UI state.
 * Brush keys are recorded with their descriptors. Raster bytes stay in @p mask_store.
 *
 * @param document Source DAG. Must have at least one Color Grade on the backbone.
 * @param mask_store Required when the document references persistent Brush keys.
 * @return Validated package with a computed fingerprint.
 * @throws std::runtime_error when the document, owners, or referenced assets fail.
 */
[[nodiscard]] auto CaptureDocumentTransfer(const PipelineDocument& document,
                                           MaskStore*              mask_store = nullptr)
    -> AdjustmentTransferPackage;

/**
 * @brief Parse a portable transfer document. Rejects operator-list packages,
 *        unknown keys, unknown versions, and non-canonical dumps.
 *
 * @throws std::runtime_error when @p json is not a canonical transfer document.
 */
[[nodiscard]] auto ImportDocumentTransfer(const nlohmann::json& json) -> AdjustmentTransferPackage;

/**
 * @brief Canonical JSON for @p package. Key order is stable. Includes fingerprint.
 */
[[nodiscard]] auto ExportDocumentTransfer(const AdjustmentTransferPackage& package)
    -> nlohmann::json;

/**
 * @brief Hash of the canonical package without the fingerprint field.
 */
[[nodiscard]] auto DocumentTransferFingerprint(const AdjustmentTransferPackage& package)
    -> std::string;

/**
 * @brief Validate package schema, Grade JSON, DRT/Post JSON, and asset descriptors.
 *
 * @throws std::runtime_error on the first failed rule. Does not mutate @p package.
 */
void ValidateDocumentTransfer(const AdjustmentTransferPackage& package);

/**
 * @brief Remap identities, copy Brush assets, and build one typed Paste batch.
 *
 * Reads @p root_document for target Develop, geometry, DRT identity, and occupied
 * IDs. Does not mutate @p root_document. Copies assets before returning. Rejects
 * ID collisions before any live document change.
 *
 * @param package Validated source package.
 * @param root_document Target immutable root DAG.
 * @param options Identity source and Mask stores.
 * @return Remapped package plus a validated Paste batch.
 * @throws std::runtime_error on validation, collision, missing asset, or graph failure.
 */
[[nodiscard]] auto PrepareDocumentPaste(const AdjustmentTransferPackage&   package,
                                        const PipelineDocument&            root_document,
                                        const DocumentTransferPasteOptions& options = {})
    -> PreparedDocumentPaste;

/**
 * @brief Testing hook that overrides a null @ref DocumentTransferPasteOptions identity.
 *
 * Pass nullptr to clear. Not used by product rendering.
 */
void SetDocumentTransferIdentitySourceForTesting(TransferIdentitySource* source);

}  // namespace alcedo
