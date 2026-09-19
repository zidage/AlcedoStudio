//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_stroke.hpp"
#endif
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

/**
 * @brief Supplies new node, adjustment, Mask, and Stroke identities during Paste remap.
 *
 * Implementations must return non-empty IDs. The paste planner rejects a value
 * that collides with a source ID, a target ID, or an earlier generated ID.
 * Callers inject a deterministic source in tests. Not thread-safe unless the
 * implementation documents otherwise.
 */
class TransferIdentitySource {
 public:
  virtual ~TransferIdentitySource()   = default;

  /** @brief Next Color Grade NodeId. Must not be empty. */
  virtual auto NextNodeId() -> NodeId = 0;

  /**
   * @brief Next adjustment instance id for @p node_id and catalog @p type.
   *
   * Default factories use `{node_id}.{suffix}`. Duplicate types on one node must
   * still yield distinct values.
   */
  virtual auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId         = 0;

  /** @brief Next MaskId. Must not be empty. */
  virtual auto NextMaskId() -> MaskId = 0;

#ifdef ALCEDO_ENABLE_BRUSH_MASK
  /** @brief Next StrokeId. Must not be empty. */
  virtual auto NextStrokeId() -> StrokeId = 0;
#endif
};

/**
 * @brief Sequential identity source for deterministic Paste tests.
 *
 * Node ids are `grade.tN`, Mask ids are `mask.tN`, and Stroke ids are `stroke.tN`
 * with N starting at 1. Adjustment ids use @ref MakeAdjustmentInstanceId.
 */
class CountingTransferIdentitySource final : public TransferIdentitySource {
 public:
  auto NextNodeId() -> NodeId override;
  auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId override;
  auto NextMaskId() -> MaskId override;
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  auto NextStrokeId() -> StrokeId override;
#endif

 private:
  std::uint32_t next_node_ = 1;
  std::uint32_t next_mask_ = 1;
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  std::uint32_t next_stroke_ = 1;
#endif
};

/**
 * @brief Prepared Paste: remapped package and typed batch.
 *
 * @p batch is validated. It is not applied to a live document.
 */
struct PreparedDocumentPaste {
  AdjustmentTransferPackage package;
  PipelineEditBatch         batch;
};

/**
 * @brief Optional Paste collaborators. Null identity uses a per-call default source
 *        or the testing hook.
 */
struct DocumentTransferPasteOptions {
  TransferIdentitySource* identity_source = nullptr;
};

/**
 * @brief Plans one root-relative Paste batch from a sparse v6 transfer package.
 *
 * Starts from the target immutable root document, materializes each selected
 * Color Grade from clean catalog defaults, applies only the selected source
 * values, applies the selected DRT/Post values to the target root DRT/Post
 * node, and remaps every transferred identity to a collision-free target
 * identity. Produces one validated @ref PipelineEditBatch. Does not mutate the
 * target root or any live document.
 */
class DocumentTransferPlanner {
 public:
  DocumentTransferPlanner() = delete;

  /**
   * @brief Remap identities and build one typed Paste batch.
   *
   * Reads @p root_document for target Develop, geometry, DRT identity, and
   * occupied IDs. Does not mutate @p root_document. Rejects ID collisions
   * before any live document change.
   *
   * Selected Color Grades start from clean defaults; only their selected
   * values are applied. An empty Color Grade set leaves the target root Grade
   * chain untouched. The remapped source default Grade stays default; when the
   * package omits the source default, the first included Grade becomes the
   * default and, with its transferred Masks, receives the required default
   * deletion protection. Other protection values keep their source values.
   *
   * @param package Validated source package.
   * @param root_document Target immutable root DAG.
   * @param options Identity source.
   * @return Remapped package plus a validated Paste batch.
   * @throws std::runtime_error on validation, collision, missing asset, or graph failure.
   */
  [[nodiscard]] static auto Plan(const AdjustmentTransferPackage&    package,
                                 const PipelineDocument&             root_document,
                                 const DocumentTransferPasteOptions& options = {})
      -> PreparedDocumentPaste;
};

/**
 * @brief Testing hook that overrides a null @ref DocumentTransferPasteOptions identity.
 *
 * Pass nullptr to clear. Not used by product rendering.
 */
void SetDocumentTransferIdentitySourceForTesting(TransferIdentitySource* source);

}  // namespace alcedo
