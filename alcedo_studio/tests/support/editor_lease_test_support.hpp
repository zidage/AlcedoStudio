//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "type/type.hpp"

namespace alcedo::test {

/**
 * @brief Editor lease of an image that exists only in memory: an empty history whose root and
 *        working document are @p root_document.
 *
 * For harnesses that drive the editor history without storage. Install it through
 * EditorSessionPipelineMappers::acquire_editor_lease. Replays start from the same document, so a
 * Version checkout reproduces the working document of the root exactly.
 */
[[nodiscard]] inline auto MakeInMemoryEditorLease(
    sl_element_id_t element_id, PipelineDocument root_document = CreateDefaultPipelineDocument())
    -> EditorHistoryLease {
  // Bind the root the way every replay does (BuildDocumentFromRoot), so the opened document and a
  // replay of the root are the same document.
  BindRootCameraProfile(root_document, std::nullopt);
  auto root =
      std::make_shared<const LoadedRootState>(LoadedRootState{std::move(root_document), {}});
  auto document = std::make_shared<PipelineDocument>(ClonePipelineDocument(root->document));
  return EditorHistoryLease{
      .graph_    = CommitGraph::CreateEmpty(element_id),
      .root_     = std::move(root),
      .document_ = std::move(document),
  };
}

}  // namespace alcedo::test
