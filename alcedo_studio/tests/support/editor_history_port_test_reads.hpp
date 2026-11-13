//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::test {

/**
 * @brief Copy of the CommitGraph that the editor history of @p element_id works on, read through
 *        IEditorHistoryPort::SnapshotHistorySource.
 *
 * Reports a test failure and returns null when the history does not hold the image.
 */
[[nodiscard]] inline auto EditorHistoryGraph(IEditorHistoryPort& history,
                                             sl_element_id_t     element_id)
    -> std::shared_ptr<const CommitGraph> {
  std::shared_ptr<const CommitGraph>      graph;
  std::shared_ptr<const PipelineDocument> root_document;
  std::string                             error;
  if (!history.SnapshotHistorySource({element_id, true}, &graph, &root_document, &error)) {
    ADD_FAILURE() << "history of image " << element_id << " is not readable: " << error;
    return nullptr;
  }
  return graph;
}

/// Working head of the active Version of the editor history of @p element_id.
[[nodiscard]] inline auto EditorWorkingHead(IEditorHistoryPort& history, sl_element_id_t element_id)
    -> head_commit_hash_t {
  const auto graph = EditorHistoryGraph(history, element_id);
  return graph ? graph->GetActiveVersionRef().head_commit_hash : std::nullopt;
}

/// Transaction chain hash of the working head of the editor history of @p element_id.
[[nodiscard]] inline auto EditorWorkingChain(IEditorHistoryPort& history,
                                             sl_element_id_t     element_id)
    -> transaction_chain_hash_t {
  const auto graph = EditorHistoryGraph(history, element_id);
  return graph ? graph->ChainHashForHead(graph->GetActiveVersionRef().head_commit_hash)
               : transaction_chain_hash_t{};
}

/**
 * @brief Working document of @p element_id as the history published it after its last
 *        operation.
 *
 * Reports a test failure and returns null when the editor holds no lease for the image.
 */
[[nodiscard]] inline auto EditorWorkingPreview(const ui::EditorSessionPipelinePort& pipeline,
                                               sl_element_id_t                      element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  auto preview = pipeline.CurrentPreview(element_id);
  if (!preview) {
    ADD_FAILURE() << "the editor holds no lease for image " << element_id;
  }
  return preview;
}

/**
 * @brief Default pipeline document with the working-space camera profile bound, as
 *        PipelineMgmtService::InitializeImageRoot binds the root of a non-RAW image.
 *
 * BuildDocumentFromRoot binds the same profile after every replay, so an in-memory lease made
 * from this root reproduces its working document exactly on a Version checkout to the root.
 */
[[nodiscard]] inline auto WorkingSpaceBoundDefaultDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  BindRootCameraProfile(document, std::nullopt);
  return document;
}

/// A lease equal to @p lease with a private working document, so every acquisition of a harness
/// starts from the same history and document.
[[nodiscard]] inline auto CopyEditorLease(const EditorHistoryLease& lease) -> EditorHistoryLease {
  return EditorHistoryLease{
      .graph_    = lease.graph_,
      .root_     = lease.root_,
      .document_ = std::make_shared<PipelineDocument>(ClonePipelineDocument(*lease.document_)),
  };
}

}  // namespace alcedo::test
