//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_preview_image.hpp"

#include <stdexcept>
#include <string>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"

namespace alcedo {

auto BuildLutPreviewDocument(const CommitGraph& graph, const LoadedRootState& root,
                             const std::shared_ptr<const PipelineGraphSnapshot>& current,
                             sl_element_id_t element_id, const EditorLutWrite& write)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  if (!current) {
    throw std::invalid_argument("The working state of the image is missing");
  }
  if (current->ElementId() != element_id) {
    throw std::invalid_argument("The working state belongs to another image");
  }
  std::string replay_error;
  auto        document = BuildDocumentFromRoot(graph, root, std::nullopt, &replay_error);
  if (!document) {
    throw std::runtime_error("The import root of the image cannot be rebuilt: " + replay_error);
  }
  ValidateProductDocument(*document, element_id);
  document->UseSensorSettingsFrom(current->Document());
  (void)document->Develop()->Params().DngProfile().RequireBound();

  auto* grade = document->PrimaryGrade();
  if (grade == nullptr) {
    throw std::invalid_argument("The import root of the image has no Color Grade for a LUT");
  }
  auto* lmt = dynamic_cast<LmtModel*>(grade->FindAdjustmentByType(type_ids::Lmt()));
  if (lmt == nullptr) {
    throw std::invalid_argument("The primary Color Grade of the import root has no LUT adjustment");
  }
  // A preview shows the complete LUT result, whatever strength the user set on the photo.
  EditorLutWrite full_strength = write;
  full_strength.strength       = kDefaultLutStrength;
  lmt->ApplyUpdate(full_strength);

  // The private document is frozen and dropped here; only the frozen copy leaves this call.
  return PipelineGraphSnapshot::Preview(document->Freeze(), current->ElementId(),
                                        current->Lineage(), transaction_chain_hash_t{});
}

auto MakeLutPreviewRenderRequest() -> RenderRequest {
  RenderRequest request;
  request.document_geometry   = DocumentGeometryUse::ApplyCropAndRotation;
  request.resolution.max_edge = kLutPreviewLongEdge;
  request.resolution.quality  = RenderQuality::Preview;
  return request;
}

auto LutPreviewOutputColor(const PipelineDocument& document)
    -> std::optional<ExportColorProfileConfig> {
  const auto* drt = document.Drt();
  if (drt != nullptr && IsHdrExportEncoding(*drt)) {
    return ExportColorProfileConfig{};
  }
  return std::nullopt;
}

}  // namespace alcedo
