//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/preset_preview_image.hpp"

#include <xxhash.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

#include "app/document_transfer_planner.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

/// The Develop fields that PipelineDocument::UseSensorSettingsFrom keeps from the preview
/// document (DevelopParamsModel::UseSensorSettingsFrom). They are not part of the sensor key.
constexpr std::array<const char*, 7> kWhiteBalanceKeys = {
    "use_camera_wb", "user_wb",     "wb_mode",      "custom_cct",
    "custom_tint",   "as_shot_cct", "as_shot_tint",
};

}  // namespace

auto BuildPresetPreviewDocument(const CommitGraph& graph, const LoadedRootState& root,
                                const std::shared_ptr<const PipelineGraphSnapshot>& current,
                                sl_element_id_t                                     element_id,
                                const AdjustmentTransferPackage&                    package)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  if (!current) {
    throw std::invalid_argument("The working state of the image is missing");
  }
  if (current->ElementId() != element_id) {
    throw std::invalid_argument("The working state belongs to another image");
  }
  if (package.Empty()) {
    throw std::invalid_argument("The preset holds no adjustment");
  }
  std::string replay_error;
  auto        document = BuildDocumentFromRoot(graph, root, std::nullopt, &replay_error);
  if (!document) {
    throw std::runtime_error("The import root of the image cannot be rebuilt: " + replay_error);
  }
  ValidateProductDocument(*document, element_id);

  // The planner of every Paste path: the same batch that an apply of the preset commits.
  const auto  prepared = DocumentTransferPlanner::Plan(package, root.document);
  std::string apply_error;
  if (!ApplyPipelineEditBatch(*document, prepared.batch, PipelineEditApplyDirection::Forward,
                              &apply_error)) {
    throw std::runtime_error("The preset cannot be applied to the import root: " + apply_error);
  }
  // The sensor stage of the current Version (plan D6): the RAW Decode values of the preset, its
  // demosaic method and AI Denoise included, are not in the preview. Its white balance is.
  document->UseSensorSettingsFrom(current->Document());
  (void)document->Develop()->Params().DngProfile().RequireBound();

  // The private document is frozen and dropped here; only the frozen copy leaves this call.
  return PipelineGraphSnapshot::Preview(document->Freeze(), current->ElementId(),
                                        current->Lineage(), transaction_chain_hash_t{});
}

auto PresetPreviewSensorKey(const PipelineDocument& current) -> std::string {
  const auto* develop = current.Develop();
  if (develop == nullptr) {
    return {};
  }
  nlohmann::json params = develop->Params().ToJson();
  for (const char* key : kWhiteBalanceKeys) {
    params.erase(key);
  }
  const std::string    text   = params.dump();
  const std::uint64_t  digest = XXH3_64bits(text.data(), text.size());
  std::array<char, 17> hex{};
  std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(digest));
  return std::string{hex.data()};
}

}  // namespace alcedo
