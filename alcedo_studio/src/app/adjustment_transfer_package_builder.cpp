//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_package_builder.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/document_transfer.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(std::string message) { throw std::runtime_error(std::move(message)); }

auto ItemKey(const AdjustmentTransferItemSelection& item) -> std::string {
  std::string key = std::to_string(static_cast<int>(item.kind));
  if (item.adjustment_id.has_value()) {
    key += "|" + std::string{item.adjustment_id->Value()};
  }
  return key;
}

void RequireNoAdjustmentIdentity(const AdjustmentTransferItemSelection& item,
                                 const NodeId& node_id) {
  if (item.adjustment_id.has_value()) {
    Fail("transfer item on node '" + std::string{node_id.Value()} +
         "' carries an adjustment identity it cannot use");
  }
}

auto RequireOwnedAdjustment(const AdjustmentTransferItemSelection& item, const NodeId& node_id,
                            const IOperatorModel* model) -> AdjustmentInstanceId {
  if (!item.adjustment_id.has_value() || item.adjustment_id->Empty()) {
    Fail("Adjustment item on node '" + std::string{node_id.Value()} +
         "' requires an AdjustmentInstanceId");
  }
  if (model == nullptr) {
    Fail("Adjustment '" + std::string{item.adjustment_id->Value()} +
         "' is not owned by node '" + std::string{node_id.Value()} + "'");
  }
  return *item.adjustment_id;
}

template <typename NodeModel>
auto SelectedAdjustmentIds(const AdjustmentTransferNodeSelection& selection,
                           const NodeModel& node) -> std::set<std::string> {
  std::set<std::string> ids;
  for (const auto& item : selection.items) {
    if (item.kind != AdjustmentTransferItemKind::Adjustment) {
      continue;
    }
    const auto id = RequireOwnedAdjustment(item, selection.node_id,
                                           node.FindAdjustment(
                                               item.adjustment_id.value_or(AdjustmentInstanceId{})));
    ids.insert(std::string{id.Value()});
  }
  return ids;
}

auto BuildColorGradeEntry(const ColorGradeNodeModel&            grade,
                          const AdjustmentTransferNodeSelection& selection)
    -> TransferColorGradeValue {
  TransferColorGradeValue entry;
  entry.source_node_id     = grade.Id();
  entry.display_name       = std::string{grade.DisplayName()};
  entry.deletion_protected = grade.DeletionProtected();

  std::set<std::string> item_keys;
  bool                  masks_selected = false;
  for (const auto& item : selection.items) {
    if (!item_keys.insert(ItemKey(item)).second) {
      Fail("duplicate transfer item on Color Grade '" + std::string{grade.Id().Value()} + "'");
    }
    switch (item.kind) {
      case AdjustmentTransferItemKind::NodeEnabled:
        RequireNoAdjustmentIdentity(item, grade.Id());
        entry.enabled = grade.Enabled();
        break;
      case AdjustmentTransferItemKind::NodeMix:
        RequireNoAdjustmentIdentity(item, grade.Id());
        entry.mix = grade.Mix();
        break;
      case AdjustmentTransferItemKind::Masks:
        RequireNoAdjustmentIdentity(item, grade.Id());
        if (grade.MaskCount() == 0) {
          Fail("Masks item selected on Color Grade '" + std::string{grade.Id().Value()} +
               "' which has no Masks");
        }
        masks_selected = true;
        break;
      case AdjustmentTransferItemKind::Adjustment:
        break;
      case AdjustmentTransferItemKind::DrtParameters:
        Fail("DrtParameters item requires the DRT/Post endpoint, not Color Grade '" +
             std::string{grade.Id().Value()} + "'");
    }
  }

  const auto selected = SelectedAdjustmentIds(selection, grade);
  for (std::size_t index = 0; index < grade.AdjustmentCount(); ++index) {
    const auto& id = grade.AdjustmentIdAt(index);
    if (!selected.contains(std::string{id.Value()})) {
      continue;
    }
    const auto& model = grade.AdjustmentAt(index);
    entry.adjustments.push_back({id, model.Type(), model.ToJson()});
  }
  if (masks_selected) {
    entry.masks = std::vector<MaskModel>{grade.Masks().begin(), grade.Masks().end()};
  }
  return entry;
}

auto BuildDrtPostEntry(const DrtNodeModel&                    drt,
                       const AdjustmentTransferNodeSelection& selection) -> TransferDrtPostValue {
  TransferDrtPostValue entry;
  std::set<std::string> item_keys;
  for (const auto& item : selection.items) {
    if (!item_keys.insert(ItemKey(item)).second) {
      Fail("duplicate transfer item on DRT/Post endpoint '" + std::string{drt.Id().Value()} + "'");
    }
    switch (item.kind) {
      case AdjustmentTransferItemKind::DrtParameters:
        RequireNoAdjustmentIdentity(item, drt.Id());
        entry.params = drt.Params().ToJson();
        break;
      case AdjustmentTransferItemKind::Adjustment:
        break;
      case AdjustmentTransferItemKind::NodeEnabled:
      case AdjustmentTransferItemKind::NodeMix:
      case AdjustmentTransferItemKind::Masks:
        Fail("transfer item kind requires a Color Grade node, not DRT/Post endpoint '" +
             std::string{drt.Id().Value()} + "'");
    }
  }

  const auto selected = SelectedAdjustmentIds(selection, drt);
  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    const auto& id = drt.AdjustmentIdAt(index);
    if (!selected.contains(std::string{id.Value()})) {
      continue;
    }
    const auto& model = drt.AdjustmentAt(index);
    entry.adjustments.push_back({id, model.Type(), model.ToJson()});
  }
  return entry;
}

}  // namespace

auto AdjustmentTransferPackageBuilder::Build(const PipelineDocument&            document,
                                             const AdjustmentTransferSelection& selection)
    -> AdjustmentTransferPackage {
  if (selection.nodes.empty()) {
    Fail("transfer selection contains no nodes");
  }

  std::map<std::string, const AdjustmentTransferNodeSelection*> by_node;
  for (const auto& node : selection.nodes) {
    if (node.node_id.Empty()) {
      Fail("transfer selection contains an empty node identity");
    }
    if (node.items.empty()) {
      Fail("transfer selection for node '" + std::string{node.node_id.Value()} +
           "' contains no items");
    }
    if (!by_node.emplace(std::string{node.node_id.Value()}, &node).second) {
      Fail("duplicate node in transfer selection: '" + std::string{node.node_id.Value()} + "'");
    }
  }

  AdjustmentTransferPackage package;
  package.schema_                  = std::string{kAdjustmentTransferSchema};
  package.document_format_version_ = document.FormatVersion();

  std::set<std::string> matched;
  for (const auto* grade : ColorGradesOnImageBackbone(document)) {
    const auto found = by_node.find(std::string{grade->Id().Value()});
    if (found == by_node.end()) {
      continue;
    }
    matched.insert(found->first);
    package.color_grades_.push_back(BuildColorGradeEntry(*grade, *found->second));
    if (grade->Id() == document.DefaultGradeId()) {
      package.default_grade_id_ = grade->Id();
    }
  }
  if (const auto* drt = document.Drt(); drt != nullptr) {
    if (const auto found = by_node.find(std::string{drt->Id().Value()});
        found != by_node.end()) {
      matched.insert(found->first);
      package.drt_post_ = BuildDrtPostEntry(*drt, *found->second);
    }
  }
  for (const auto& [id, node] : by_node) {
    (void)node;
    if (!matched.contains(id)) {
      Fail("transfer selection node '" + id + "' is not a transferable backbone node");
    }
  }
  if (package.Empty()) {
    Fail("transfer selection contains no transferable items");
  }
  ValidateDocumentTransfer(package);
  package.fingerprint_ = DocumentTransferFingerprint(package);
  return package;
}

auto SelectAllTransferableItems(const PipelineDocument& document) -> AdjustmentTransferSelection {
  AdjustmentTransferSelection selection;
  for (const auto* grade : ColorGradesOnImageBackbone(document)) {
    AdjustmentTransferNodeSelection node;
    node.node_id = grade->Id();
    node.items.push_back({AdjustmentTransferItemKind::NodeEnabled, std::nullopt});
    node.items.push_back({AdjustmentTransferItemKind::NodeMix, std::nullopt});
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      node.items.push_back(
          {AdjustmentTransferItemKind::Adjustment, grade->AdjustmentIdAt(index)});
    }
    if (grade->MaskCount() > 0) {
      node.items.push_back({AdjustmentTransferItemKind::Masks, std::nullopt});
    }
    selection.nodes.push_back(std::move(node));
  }
  if (const auto* drt = document.Drt(); drt != nullptr) {
    AdjustmentTransferNodeSelection node;
    node.node_id = drt->Id();
    node.items.push_back({AdjustmentTransferItemKind::DrtParameters, std::nullopt});
    for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
      node.items.push_back(
          {AdjustmentTransferItemKind::Adjustment, drt->AdjustmentIdAt(index)});
    }
    selection.nodes.push_back(std::move(node));
  }
  return selection;
}

}  // namespace alcedo
