//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_graph_snapshot.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace alcedo {

auto PipelineLineageId::Next() -> PipelineLineageId {
  static std::atomic<std::uint64_t> last{0};
  return PipelineLineageId{last.fetch_add(1, std::memory_order_relaxed) + 1};
}

PipelineGraphSnapshot::PipelineGraphSnapshot(std::shared_ptr<const PipelineDocument> document,
                                             sl_element_id_t element_id,
                                             PipelineLineageId lineage, head_commit_hash_t head,
                                             transaction_chain_hash_t chain, bool committed)
    : document_(std::move(document)),
      element_id_(element_id),
      lineage_(lineage),
      head_(std::move(head)),
      chain_(chain),
      committed_(committed) {
  if (document_ == nullptr) {
    throw std::invalid_argument("PipelineGraphSnapshot requires a document");
  }
  if (lineage_.Empty()) {
    throw std::invalid_argument("PipelineGraphSnapshot requires a lineage");
  }
}

auto PipelineGraphSnapshot::Committed(std::shared_ptr<const PipelineDocument> document,
                                      sl_element_id_t element_id, PipelineLineageId lineage,
                                      head_commit_hash_t head, transaction_chain_hash_t chain)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  return std::shared_ptr<const PipelineGraphSnapshot>(new PipelineGraphSnapshot(
      std::move(document), element_id, lineage, std::move(head), chain, true));
}

auto PipelineGraphSnapshot::Preview(std::shared_ptr<const PipelineDocument> document,
                                    sl_element_id_t element_id, PipelineLineageId lineage,
                                    transaction_chain_hash_t chain)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  return std::shared_ptr<const PipelineGraphSnapshot>(new PipelineGraphSnapshot(
      std::move(document), element_id, lineage, std::nullopt, chain, false));
}

}  // namespace alcedo
