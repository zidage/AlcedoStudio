//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "type/type.hpp"

namespace alcedo::test {

/**
 * @brief Test owner of one working document that renderers receive as frozen snapshots.
 *
 * Stands in for the document owner (editor session or PipelineGuard) in renderer and executor
 * tests. Every @ref Freeze returns a preview snapshot of the document's current values with the
 * same lineage, like successive frames of one loaded history, so an interactive renderer keeps
 * its binding. @ref Rebind replaces the document and takes a new lineage, like a reload or a
 * Version checkout, so the next interactive render releases the previous binding.
 *
 * Thread: write the document and call Freeze on one thread, or order them with one lock.
 */
class RenderSnapshotSource {
 public:
  explicit RenderSnapshotSource(std::shared_ptr<PipelineDocument> document,
                                sl_element_id_t                   element_id = 1)
      : document_(std::move(document)),
        element_id_(element_id),
        lineage_(PipelineLineageId::Next()) {
    if (!document_) {
      throw std::invalid_argument("RenderSnapshotSource: document is null");
    }
  }

  /// Snapshot of the current document values in the current lineage.
  [[nodiscard]] auto Freeze() const -> std::shared_ptr<const PipelineGraphSnapshot> {
    return PipelineGraphSnapshot::Preview(document_->Freeze(), element_id_, lineage_,
                                          transaction_chain_hash_t{});
  }

  /// Source for PipelineTask::snapshot_under_render_lock_. Freezes this owner's document at
  /// the time the scheduler calls it; the owner must outlive the task.
  [[nodiscard]] auto TaskSource() const
      -> std::function<std::shared_ptr<const PipelineGraphSnapshot>()> {
    return [this]() { return Freeze(); };
  }

  /// Replace the document and take a new lineage.
  void Rebind(std::shared_ptr<PipelineDocument> document) {
    if (!document) {
      throw std::invalid_argument("RenderSnapshotSource: document is null");
    }
    document_ = std::move(document);
    lineage_  = PipelineLineageId::Next();
  }

  [[nodiscard]] auto Document() const -> PipelineDocument& { return *document_; }
  [[nodiscard]] auto SharedDocument() const -> const std::shared_ptr<PipelineDocument>& {
    return document_;
  }
  [[nodiscard]] auto Lineage() const -> PipelineLineageId { return lineage_; }
  [[nodiscard]] auto ElementId() const -> sl_element_id_t { return element_id_; }

 private:
  std::shared_ptr<PipelineDocument> document_;
  sl_element_id_t                   element_id_ = 0;
  PipelineLineageId                 lineage_;
};

/// One preview snapshot of @p document in a new lineage; for single-render tests.
[[nodiscard]] inline auto FreezeInNewLineage(const PipelineDocument& document,
                                             sl_element_id_t        element_id = 1)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  return PipelineGraphSnapshot::Preview(document.Freeze(), element_id, PipelineLineageId::Next(),
                                        transaction_chain_hash_t{});
}

}  // namespace alcedo::test
