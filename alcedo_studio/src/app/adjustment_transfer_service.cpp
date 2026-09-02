//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_service.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "edit/history/edit_commit.hpp"

namespace alcedo {
namespace {

auto UniqueVersionDisplayNameForGraph(const CommitGraph& graph, std::string requested,
                                      std::string fallback) -> std::string {
  auto base_name = requested.empty() ? std::move(fallback) : std::move(requested);
  bool base_exists = false;
  int  max_suffix  = 1;
  for (const auto& [id, ref] : graph.GetAllVersionRefs()) {
    (void)id;
    const auto& name = ref.display_name;
    if (name == base_name) {
      base_exists = true;
      continue;
    }
    const auto prefix = base_name + " (";
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        name.back() != ')') {
      continue;
    }
    try {
      const auto parsed = std::stoi(name.substr(prefix.size(), name.size() - prefix.size() - 1));
      if (parsed > 1) {
        max_suffix  = std::max(max_suffix, parsed);
        base_exists = true;
      }
    } catch (...) {
    }
  }
  if (!base_exists) {
    return base_name;
  }
  return base_name + " (" + std::to_string(max_suffix + 1) + ")";
}

}  // namespace

auto AdjustmentTransferService::Capture(const PipelineDocument& document, MaskStore* mask_store)
    -> AdjustmentTransferPackage {
  return CaptureDocumentTransfer(document, mask_store);
}

auto AdjustmentTransferService::ImportPackage(const nlohmann::json& package_json)
    -> AdjustmentTransferPackage {
  return ImportDocumentTransfer(package_json);
}

auto AdjustmentTransferService::ExportPackage(const AdjustmentTransferPackage& package)
    -> nlohmann::json {
  return ExportDocumentTransfer(package);
}

auto AdjustmentTransferService::PackageFingerprint(const AdjustmentTransferPackage& package)
    -> std::string {
  return DocumentTransferFingerprint(package);
}

auto AdjustmentTransferService::PasteAsRootRelativeVersion(
    CommitGraph& graph, const PipelineDocument& root_document,
    const AdjustmentTransferPackage& package, std::string version_display_name,
    const DocumentTransferPasteOptions& options) -> AdjustmentPasteResult {
  AdjustmentPasteResult result;
  result.prior_version_id = graph.GetActiveVersionId();
  PreparedDocumentPaste prepared;
  try {
    prepared = PrepareDocumentPaste(package, root_document, options);
  } catch (const std::exception& ex) {
    result.error = ex.what();
    return result;
  }

  version_ref_id_t new_version_id{};
  bool             version_created = false;
  try {
    const auto display_name =
        UniqueVersionDisplayNameForGraph(graph, std::move(version_display_name),
                                         "Pasted Adjustments");
    new_version_id = graph.CreateVersionRefAtRoot(display_name);
    version_created = true;
    graph.SetActiveVersionId(new_version_id);
    auto commit =
        EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt, std::move(prepared.batch));
    if (!graph.InsertCommit(commit)) {
      throw std::runtime_error("Paste commit was not inserted");
    }
    graph.MoveWorkingHead(new_version_id, commit.GetCommitHash());
    result.pasted         = true;
    result.new_version_id = new_version_id;
    result.new_head       = commit.GetCommitHash();
    return result;
  } catch (const std::exception& ex) {
    graph.SetActiveVersionId(result.prior_version_id);
    if (version_created) {
      (void)graph.RemoveVersionRef(new_version_id);
    }
    result.error = ex.what();
    return result;
  }
}

}  // namespace alcedo
