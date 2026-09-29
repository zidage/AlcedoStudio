//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_working_document.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

namespace alcedo {

EditorWorkingDocument::EditorWorkingDocument(sl_element_id_t                   element_id,
                                             std::shared_ptr<PipelineDocument> document)
    : element_id_(element_id), document_(std::move(document)), lineage_(PipelineLineageId::Next()) {
  if (!document_) {
    throw std::invalid_argument("EditorWorkingDocument: document is null");
  }
  (void)PublishPreview();
}

void EditorWorkingDocument::Replace(std::shared_ptr<PipelineDocument> document) {
  if (!document) {
    throw std::invalid_argument("EditorWorkingDocument: replacement document is null");
  }
  document_ = std::move(document);
  lineage_  = PipelineLineageId::Next();
}

auto EditorWorkingDocument::PublishPreview() -> std::shared_ptr<const PipelineGraphSnapshot> {
  // The working values may not equal any history state, so the preview carries no HEAD and an
  // empty chain.
  auto preview = PipelineGraphSnapshot::Preview(document_->Freeze(), element_id_, lineage_,
                                                transaction_chain_hash_t{});
  std::scoped_lock lock(published_mutex_);
#ifndef NDEBUG
  assert((published_ == nullptr ||
          DocumentRevisionFingerprint(published_->Document()) == published_fingerprint_) &&
         "A write reached a frozen PipelineDocument");
  published_fingerprint_ = DocumentRevisionFingerprint(preview->Document());
#endif
  published_ = preview;
  return preview;
}

auto EditorWorkingDocument::CurrentPreview() const -> std::shared_ptr<const PipelineGraphSnapshot> {
  std::scoped_lock lock(published_mutex_);
  return published_;
}

}  // namespace alcedo
