//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <compare>
#include <cstdint>
#include <memory>

#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Identity of one load of one image's pipeline history.
 *
 * A new value is taken each time an image's history is loaded or rebuilt (open, reload,
 * Version checkout). Two snapshots with the same lineage describe successive states of the
 * same loaded history; a different lineage means the history was replaced. Executors use the
 * lineage in their binding key: a lineage change releases every GPU resource bound to the
 * previous one. Values are unique in the process and are never persisted.
 */
class PipelineLineageId {
 public:
  /// Empty identity; no loaded history has it.
  PipelineLineageId() = default;

  /// Take a new process-unique identity. Thread-safe.
  [[nodiscard]] static auto Next() -> PipelineLineageId;

  [[nodiscard]] auto Value() const -> std::uint64_t { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_ == 0; }

  friend auto operator==(const PipelineLineageId&, const PipelineLineageId&) -> bool = default;
  friend auto operator<=>(const PipelineLineageId&, const PipelineLineageId&) = default;

 private:
  explicit PipelineLineageId(std::uint64_t value) : value_(value) {}

  std::uint64_t value_ = 0;
};

/**
 * @brief Immutable pipeline graph of one image at one point, with its history identity.
 *
 * Purpose: the unit that document owners hand to executors and background consumers
 * (thumbnails, analysis, export) in place of a mutable document behind a shared lock. It is
 * not compiled; each executor compiles it for its own decoded source.
 *
 * Captured state: a frozen document (@ref PipelineDocument::Freeze, nodes shared with the
 * working document, never written), the element, the lineage, and for a committed snapshot
 * the HEAD commit and transaction chain that the document equals.
 *
 * Two kinds:
 * - committed: equals the history state at @ref Head / @ref Chain. The only kind that
 *   non-editor consumers may receive; disk cache keys and analysis labels use @ref Head and
 *   @ref Chain.
 * - preview: the editor's working values, which may include uncommitted edits. Flows only
 *   between the editor session and the editor executor. @ref Head is always empty.
 *
 * Owner: whoever created it (the editor session or the pipeline service). Holders share it
 * through `shared_ptr<const PipelineGraphSnapshot>`; it is released with its last holder.
 * Consistency: the document is immutable, so readers on any thread need no lock.
 */
class PipelineGraphSnapshot {
 public:
  /**
   * @brief Snapshot of committed history state.
   * @param head HEAD commit the document equals; empty for a history that has only its root.
   * @throws std::invalid_argument when @p document is null or @p lineage is empty.
   */
  [[nodiscard]] static auto Committed(std::shared_ptr<const PipelineDocument> document,
                                      sl_element_id_t element_id, PipelineLineageId lineage,
                                      head_commit_hash_t       head,
                                      transaction_chain_hash_t chain)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /**
   * @brief Snapshot of the editor's working values, which may be uncommitted.
   * @param chain Transaction chain of the history state the working values started from.
   * @throws std::invalid_argument when @p document is null or @p lineage is empty.
   */
  [[nodiscard]] static auto Preview(std::shared_ptr<const PipelineDocument> document,
                                    sl_element_id_t element_id, PipelineLineageId lineage,
                                    transaction_chain_hash_t chain)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  [[nodiscard]] auto Document() const -> const PipelineDocument& { return *document_; }
  [[nodiscard]] auto SharedDocument() const -> const std::shared_ptr<const PipelineDocument>& {
    return document_;
  }
  [[nodiscard]] auto ElementId() const -> sl_element_id_t { return element_id_; }
  [[nodiscard]] auto Lineage() const -> PipelineLineageId { return lineage_; }
  /// HEAD of a committed snapshot; always empty for a preview.
  [[nodiscard]] auto Head() const -> const head_commit_hash_t& { return head_; }
  [[nodiscard]] auto Chain() const -> const transaction_chain_hash_t& { return chain_; }
  [[nodiscard]] auto IsCommitted() const -> bool { return committed_; }

 private:
  PipelineGraphSnapshot(std::shared_ptr<const PipelineDocument> document,
                        sl_element_id_t element_id, PipelineLineageId lineage,
                        head_commit_hash_t head, transaction_chain_hash_t chain, bool committed);

  std::shared_ptr<const PipelineDocument> document_;
  sl_element_id_t                         element_id_ = 0;
  PipelineLineageId                       lineage_;
  head_commit_hash_t                      head_;
  transaction_chain_hash_t                chain_;
  bool                                    committed_ = false;
};

}  // namespace alcedo
