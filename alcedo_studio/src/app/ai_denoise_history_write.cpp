//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_denoise_history_write.hpp"

#include <stdexcept>
#include <utility>

#include "app/editor_parameter_write.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

constexpr auto    kRawDecodeField = "raw_decode";

[[noreturn]] void Fail(sl_element_id_t element_id, const std::string& detail) {
  throw std::runtime_error("AI Denoise: the switch of image " + std::to_string(element_id) +
                           " was not written: " + detail);
}

}  // namespace

auto WriteAiDenoiseSwitchToStoredHistory(PipelineMgmtService& pipelines, sl_element_id_t element_id,
                                         const std::optional<DevelopAiDenoise>& denoise)
    -> AiDenoiseHistoryWrite {
  const auto  base  = pipelines.LoadHistorySnapshot(element_id);
  CommitGraph graph = *base.graph_;
  const auto  head  = graph.GetActiveVersionRef().head_commit_hash;

  std::string error;
  auto        document = BuildDocumentFromRoot(graph, *base.root_, head, &error);
  if (!document) {
    Fail(element_id, "the replay of the active Version failed: " + error);
  }
  auto target = CompleteCurrentPanelParameterTarget(*document, kRawDecodeField, &error);
  if (!target.has_value()) {
    Fail(element_id, error);
  }
  nlohmann::json before;
  if (!ReadEditorParameterJson(*document, *target, &before, &error)) {
    Fail(element_id, error);
  }
  DevelopRawDecodeUpdate update;
  update.ai_denoise = denoise;
  if (!ApplyEditorParameterWrite(*document, *target, EditorParameterWrite{update}, &error)) {
    Fail(element_id, error);
  }
  nlohmann::json after;
  if (!ReadEditorParameterJson(*document, *target, &after, &error)) {
    Fail(element_id, error);
  }

  AiDenoiseHistoryWrite result;
  result.version_name = graph.GetActiveVersionRef().display_name;
  if (before == after) {
    if (head.has_value()) {
      if (const auto* commit = graph.FindCommit(*head)) {
        result.head_created_at_ns = static_cast<std::int64_t>(commit->GetCreatedAtNs());
      }
    }
    return result;
  }

  std::string node_name;
  if (const auto* node = document->Graph().FindNode(target->node_id)) {
    node_name = std::string{node->DisplayName()};
  }
  // The batch of the editor switch (DN7): one SetParameter change of the raw_decode field.
  auto commit = EditCommit::MakePipelineEdit(
      graph.GetRootId(), head,
      MakeSetParameterBatch(*target, std::move(before), std::move(after), true, true,
                            std::move(node_name)));
  const auto new_head       = commit.GetCommitHash();
  result.head_created_at_ns = static_cast<std::int64_t>(commit.GetCreatedAtNs());
  if (!graph.InsertCommit(std::move(commit))) {
    Fail(element_id, "the commit was not inserted");
  }
  graph.MoveWorkingHead(graph.GetActiveVersionId(), new_head);
  (void)pipelines.PersistHistory(base, graph);
  result.changed = true;
  return result;
}

}  // namespace alcedo
