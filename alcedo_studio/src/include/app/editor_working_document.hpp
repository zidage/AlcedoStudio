//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Working pipeline document of the image open in the editor, and the preview snapshots
 *        published from it.
 *
 * Owner: the editor session. Its history working state writes the document on the session owner
 * thread, which is the only writer, so writes take no lock. Readers on other threads never read
 * the document: the editor render dispatch and the GUI read the last published preview
 * (@ref CurrentPreview), which is immutable (PipelineDocument::Freeze shares every unchanged
 * node with the working document).
 *
 * Lineage: a new PipelineLineageId is taken at construction and on each @ref Replace (Version
 * checkout, Paste, replay). The editor executor releases every resource of the previous lineage
 * on the first frame of a new one.
 *
 * Thread: @ref Document, @ref Replace, @ref Lineage, and @ref PublishPreview run on the session
 * owner thread only. @ref CurrentPreview and @ref ElementId are safe on any thread.
 */
class EditorWorkingDocument {
 public:
  /**
   * @brief Take @p document as the working document of @p element_id and publish its preview.
   * @throws std::invalid_argument when @p document is null.
   */
  EditorWorkingDocument(sl_element_id_t element_id, std::shared_ptr<PipelineDocument> document);

  EditorWorkingDocument(const EditorWorkingDocument&)                                  = delete;
  auto               operator=(const EditorWorkingDocument&) -> EditorWorkingDocument& = delete;

  [[nodiscard]] auto ElementId() const -> sl_element_id_t { return element_id_; }

  /// The working document. Owner thread only.
  [[nodiscard]] auto Document() -> PipelineDocument& { return *document_; }
  [[nodiscard]] auto Document() const -> const PipelineDocument& { return *document_; }

  /// History identity of the working document. Owner thread only.
  [[nodiscard]] auto Lineage() const -> PipelineLineageId { return lineage_; }

  /**
   * @brief Make @p document the working document and take a new lineage.
   *
   * The swap step of build-then-swap: callers build and validate @p document first. It does not
   * publish; the next @ref PublishPreview does.
   * @throws std::invalid_argument when @p document is null; the working document is unchanged.
   */
  void               Replace(std::shared_ptr<PipelineDocument> document);

  /**
   * @brief Freeze the working document and publish it as the current preview snapshot.
   *
   * Called by the history after each write, before a render or a change notification can read
   * the result. Owner thread only.
   * @return The published preview.
   */
  auto               PublishPreview() -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// Last published preview snapshot. Never null after construction. Any thread.
  [[nodiscard]] auto CurrentPreview() const -> std::shared_ptr<const PipelineGraphSnapshot>;

 private:
  const sl_element_id_t                        element_id_;
  std::shared_ptr<PipelineDocument>            document_;
  PipelineLineageId                            lineage_;

  /// Guards only the published pointer, never held across a freeze or a render.
  mutable std::mutex                           published_mutex_;
  std::shared_ptr<const PipelineGraphSnapshot> published_;
  /// DocumentRevisionFingerprint of the published document when it was frozen. Debug builds check
  /// it before each replacement: a write that reached a frozen document is a copy-on-write defect.
  std::uint64_t                                published_fingerprint_ = 0;
};

}  // namespace alcedo
