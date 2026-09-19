//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"

#include "app/adjustment_transfer_package_builder.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

auto ParseVersionId(const QString& text, alcedo::version_ref_id_t* out) -> bool {
  if (out == nullptr || text.size() != 32) {
    return false;
  }
  try {
    *out = alcedo::version_ref_id_t::FromString(text.toStdString());
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

AdjustmentTransferDialogModel::AdjustmentTransferDialogModel(QObject* parent)
    : QObject(parent),
      versions_(new AdjustmentTransferVersionListModel(this)),
      nodes_(new AdjustmentTransferNodeListModel(this)),
      items_(new AdjustmentTransferItemListModel(this)) {}

auto AdjustmentTransferDialogModel::OpenSource(
    std::shared_ptr<const alcedo::CommitGraph>      graph,
    std::shared_ptr<const alcedo::PipelineDocument> root_document, std::string* error) -> bool {
  graph_         = std::move(graph);
  root_document_ = std::move(root_document);
  read_.reset();
  node_states_.clear();
  focused_node_ = 0;
  error_text_.clear();
  versions_->SetRows({}, {});
  nodes_->SetRows({});
  items_->SetRows({});

  if (error != nullptr) {
    error->clear();
  }
  if (graph_ == nullptr || root_document_ == nullptr) {
    if (error != nullptr) {
      *error = Tr("No source versions are available.").toStdString();
    }
    emit StateChanged();
    return false;
  }

  const auto version_rows = alcedo::AdjustmentTransferCatalogService::ListVersions(*graph_);
  if (version_rows.empty()) {
    if (error != nullptr) {
      *error = Tr("No source versions are available.").toStdString();
    }
    emit StateChanged();
    return false;
  }

  alcedo::version_ref_id_t initial_id = version_rows.front().version_id;
  for (const auto& descriptor : version_rows) {
    if (descriptor.active) {
      initial_id = descriptor.version_id;
      break;
    }
  }
  versions_->SetRows(version_rows, initial_id);

  std::string read_error;
  auto        read = alcedo::AdjustmentTransferCatalogService::ReadVersion(*graph_, *root_document_,
                                                                           initial_id, &read_error);
  if (!read.has_value()) {
    // Open with an empty node column and the replay error visible; the user
    // can still pick another Version from the list.
    SetError(QString::fromStdString(read_error));
    emit StateChanged();
    return true;
  }
  ApplyRead(std::move(*read));
  return true;
}

void AdjustmentTransferDialogModel::SelectVersion(const QString& versionId) {
  if (graph_ == nullptr || root_document_ == nullptr) {
    return;
  }
  alcedo::version_ref_id_t requested_id;
  if (!ParseVersionId(versionId, &requested_id)) {
    SetError(Tr("Source Version identity is invalid."));
    return;
  }
  if (read_.has_value() && read_->version.version_id == requested_id) {
    return;
  }
  std::string read_error;
  auto        read = alcedo::AdjustmentTransferCatalogService::ReadVersion(*graph_, *root_document_,
                                                                           requested_id, &read_error);
  if (!read.has_value()) {
    // Keep the prior valid Version selection, rows, and document.
    SetError(QString::fromStdString(read_error));
    return;
  }
  error_text_.clear();
  versions_->SetSelected(requested_id);
  ApplyRead(std::move(*read));
}

void AdjustmentTransferDialogModel::FocusNode(const QString& nodeId) {
  const auto index = FindNode(alcedo::NodeId{nodeId.toStdString()});
  if (!index.has_value() || *index == focused_node_ || !read_.has_value()) {
    return;
  }
  const auto previous = focused_node_;
  focused_node_       = *index;
  nodes_->UpdateRow(static_cast<int>(previous), NodeCheckState(node_states_[previous]), false);
  nodes_->UpdateRow(static_cast<int>(focused_node_), NodeCheckState(node_states_[focused_node_]),
                    true);
  PublishFocusedItems();
  emit StateChanged();
}

void AdjustmentTransferDialogModel::SetNodeChecked(const QString& nodeId, bool checked) {
  const auto index = FindNode(alcedo::NodeId{nodeId.toStdString()});
  if (!index.has_value()) {
    return;
  }
  SetNodeItemsChecked(*index, checked);
}

void AdjustmentTransferDialogModel::SetItemChecked(const QString& nodeId, const QString& itemKey,
                                                   bool checked) {
  const auto node_index = FindNode(alcedo::NodeId{nodeId.toStdString()});
  if (!node_index.has_value()) {
    return;
  }
  auto& node = node_states_[*node_index];
  for (std::size_t item_index = 0; item_index < node.items.size(); ++item_index) {
    auto& item = node.items[item_index];
    if (item.key != itemKey || !item.descriptor.enabled) {
      continue;
    }
    item.checked = checked;
    nodes_->UpdateRow(static_cast<int>(*node_index), NodeCheckState(node),
                      *node_index == focused_node_);
    if (*node_index == focused_node_) {
      items_->SetChecked(static_cast<int>(item_index), checked);
    }
    PublishDerivedStates();
    return;
  }
}

void AdjustmentTransferDialogModel::SetAllNodesChecked(bool checked) {
  if (node_states_.empty()) {
    return;
  }
  for (auto& node : node_states_) {
    for (auto& item : node.items) {
      if (item.descriptor.enabled) {
        item.checked = checked;
      }
    }
  }
  nodes_->SetAllCheckStates(checked ? Qt::Checked : Qt::Unchecked);
  if (focused_node_ < node_states_.size()) {
    items_->SetAllChecked(checked);
  }
  PublishDerivedStates();
}

void AdjustmentTransferDialogModel::SetAllFocusedNodeItemsChecked(bool checked) {
  if (focused_node_ >= node_states_.size()) {
    return;
  }
  SetNodeItemsChecked(focused_node_, checked);
}

void AdjustmentTransferDialogModel::ClearAll() { SetAllNodesChecked(false); }

void AdjustmentTransferDialogModel::ClearFocusedNode() { SetAllFocusedNodeItemsChecked(false); }

auto AdjustmentTransferDialogModel::selected_version_id() const -> QString {
  if (!read_.has_value()) {
    return {};
  }
  return QString::fromStdString(read_->version.version_id.ToString());
}

auto AdjustmentTransferDialogModel::selected_version_name() const -> QString {
  if (!read_.has_value()) {
    return {};
  }
  return QString::fromStdString(read_->version.display_name);
}

auto AdjustmentTransferDialogModel::focused_node_id() const -> QString {
  if (!read_.has_value() || focused_node_ >= node_states_.size()) {
    return {};
  }
  return QString::fromStdString(
      std::string{node_states_[focused_node_].descriptor.node_id.Value()});
}

auto AdjustmentTransferDialogModel::focused_node_name() const -> QString {
  if (!read_.has_value() || focused_node_ >= node_states_.size()) {
    return {};
  }
  return QString::fromStdString(node_states_[focused_node_].descriptor.display_name);
}

auto AdjustmentTransferDialogModel::can_copy() const -> bool {
  if (!read_.has_value()) {
    return false;
  }
  for (const auto& node : node_states_) {
    for (const auto& item : node.items) {
      if (item.descriptor.enabled && item.checked) {
        return true;
      }
    }
  }
  return false;
}

auto AdjustmentTransferDialogModel::all_nodes_check_state() const -> int {
  std::size_t checkable = 0;
  std::size_t checked   = 0;
  for (const auto& node : node_states_) {
    for (const auto& item : node.items) {
      if (!item.descriptor.enabled) {
        continue;
      }
      ++checkable;
      if (item.checked) {
        ++checked;
      }
    }
  }
  if (checked == 0) {
    return Qt::Unchecked;
  }
  return checked == checkable ? Qt::Checked : Qt::PartiallyChecked;
}

auto AdjustmentTransferDialogModel::focused_items_check_state() const -> int {
  if (focused_node_ >= node_states_.size()) {
    return Qt::Unchecked;
  }
  return NodeCheckState(node_states_[focused_node_]);
}

auto AdjustmentTransferDialogModel::BuildSelection() const -> alcedo::AdjustmentTransferSelection {
  alcedo::AdjustmentTransferSelection selection;
  if (!read_.has_value()) {
    return selection;
  }
  selection.source_version_id = read_->version.version_id;
  for (const auto& node : node_states_) {
    alcedo::AdjustmentTransferNodeSelection node_selection;
    node_selection.node_id = node.descriptor.node_id;
    for (const auto& item : node.items) {
      if (!item.descriptor.enabled || !item.checked) {
        continue;
      }
      node_selection.items.push_back(alcedo::AdjustmentTransferItemSelection{
          item.descriptor.kind, item.descriptor.adjustment_id});
    }
    if (!node_selection.items.empty()) {
      selection.nodes.push_back(std::move(node_selection));
    }
  }
  return selection;
}

auto AdjustmentTransferDialogModel::BuildPackage(std::string* error) const
    -> std::optional<alcedo::AdjustmentTransferPackage> {
  if (error != nullptr) {
    error->clear();
  }
  if (!read_.has_value()) {
    if (error != nullptr) {
      *error = Tr("No source Version is loaded.").toStdString();
    }
    return std::nullopt;
  }
  const auto selection = BuildSelection();
  if (selection.nodes.empty()) {
    if (error != nullptr) {
      *error = Tr("No transferable adjustments.").toStdString();
    }
    return std::nullopt;
  }
  try {
    return alcedo::AdjustmentTransferPackageBuilder::Build(read_->document, selection);
  } catch (const std::exception& e) {
    if (error != nullptr) {
      *error = e.what();
    }
    return std::nullopt;
  } catch (...) {
    if (error != nullptr) {
      *error = Tr("Adjustment package validation failed.").toStdString();
    }
    return std::nullopt;
  }
}

auto AdjustmentTransferDialogModel::SelectionSummary() const -> QVariantList {
  QVariantList rows;
  if (!read_.has_value()) {
    return rows;
  }
  int node_index = 0;
  for (const auto& node : node_states_) {
    const auto node_name = QString::fromStdString(node.descriptor.display_name);
    for (const auto& item : node.items) {
      if (!item.descriptor.enabled || !item.checked) {
        continue;
      }
      rows.push_back(QVariantMap{
          {"key", item.key},
          {"node", node_index},
          {"section", node_name},
          {"label", QString::fromStdString(item.descriptor.display_name)},
          {"value", QString::fromStdString(item.descriptor.display_value)},
          {"itemSection", static_cast<int>(item.descriptor.section)},
          {"itemKind", static_cast<int>(item.descriptor.kind)},
          {"checked", true},
      });
    }
    ++node_index;
  }
  return rows;
}

auto AdjustmentTransferDialogModel::NodeCheckStateForTesting(const alcedo::NodeId& node_id) const
    -> int {
  const auto index = FindNode(node_id);
  if (!index.has_value()) {
    return Qt::Unchecked;
  }
  return NodeCheckState(node_states_[*index]);
}

// --- Private helpers --------------------------------------------------------

void AdjustmentTransferDialogModel::ApplyRead(alcedo::AdjustmentTransferCatalogRead read) {
  read_ = std::move(read);
  node_states_.clear();
  node_states_.reserve(read_->nodes.size());
  for (const auto& descriptor : read_->nodes) {
    NodeState state;
    state.descriptor = descriptor;
    state.items.reserve(descriptor.items.size());
    for (const auto& item : descriptor.items) {
      state.items.push_back(ItemState{item, ItemKeyFor(item), item.enabled});
    }
    node_states_.push_back(std::move(state));
  }
  focused_node_ = 0;
  PublishNodeRows();
  PublishFocusedItems();
  PublishDerivedStates();
}

void AdjustmentTransferDialogModel::PublishNodeRows() {
  std::vector<AdjustmentTransferNodeListModel::Row> rows;
  rows.reserve(node_states_.size());
  for (std::size_t index = 0; index < node_states_.size(); ++index) {
    rows.push_back(NodeRowFor(node_states_[index], index == focused_node_));
  }
  nodes_->SetRows(std::move(rows));
}

void AdjustmentTransferDialogModel::PublishFocusedItems() {
  std::vector<AdjustmentTransferItemListModel::Row> rows;
  if (focused_node_ < node_states_.size()) {
    const auto& node = node_states_[focused_node_];
    rows.reserve(node.items.size());
    for (const auto& item : node.items) {
      rows.push_back(AdjustmentTransferItemListModel::Row{
          item.key,
          QString::fromStdString(item.descriptor.display_name),
          QString::fromStdString(item.descriptor.display_value),
          static_cast<int>(item.descriptor.section),
          static_cast<int>(item.descriptor.kind),
          item.checked,
          item.descriptor.enabled,
      });
    }
  }
  items_->SetRows(std::move(rows));
}

void AdjustmentTransferDialogModel::PublishDerivedStates() { emit StateChanged(); }

void AdjustmentTransferDialogModel::SetNodeItemsChecked(std::size_t node_index, bool checked) {
  if (node_index >= node_states_.size()) {
    return;
  }
  auto& node = node_states_[node_index];
  for (auto& item : node.items) {
    if (item.descriptor.enabled) {
      item.checked = checked;
    }
  }
  nodes_->UpdateRow(static_cast<int>(node_index), NodeCheckState(node),
                    node_index == focused_node_);
  if (node_index == focused_node_) {
    items_->SetAllChecked(checked);
  }
  PublishDerivedStates();
}

void AdjustmentTransferDialogModel::SetError(const QString& text) {
  error_text_ = text;
  emit StateChanged();
}

auto AdjustmentTransferDialogModel::FindNode(const alcedo::NodeId& id) const
    -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < node_states_.size(); ++index) {
    if (node_states_[index].descriptor.node_id == id) {
      return index;
    }
  }
  return std::nullopt;
}

auto AdjustmentTransferDialogModel::ItemKeyFor(
    const alcedo::AdjustmentTransferItemDescriptor& descriptor) -> QString {
  switch (descriptor.kind) {
    case alcedo::AdjustmentTransferItemKind::NodeEnabled:
      return QStringLiteral("enabled");
    case alcedo::AdjustmentTransferItemKind::NodeMix:
      return QStringLiteral("mix");
    case alcedo::AdjustmentTransferItemKind::Masks:
      return QStringLiteral("masks");
    case alcedo::AdjustmentTransferItemKind::DrtParameters:
      return QStringLiteral("drt-params");
    case alcedo::AdjustmentTransferItemKind::Adjustment:
      if (descriptor.adjustment_id.has_value()) {
        return QStringLiteral("adj:") +
               QString::fromStdString(std::string{descriptor.adjustment_id->Value()});
      }
      return QStringLiteral("adj:");
  }
  return {};
}

auto AdjustmentTransferDialogModel::NodeCheckState(const NodeState& node) -> int {
  std::size_t checkable = 0;
  std::size_t checked   = 0;
  for (const auto& item : node.items) {
    if (!item.descriptor.enabled) {
      continue;
    }
    ++checkable;
    if (item.checked) {
      ++checked;
    }
  }
  if (checked == 0) {
    return Qt::Unchecked;
  }
  return checked == checkable ? Qt::Checked : Qt::PartiallyChecked;
}

auto AdjustmentTransferDialogModel::NodeRowFor(const NodeState& node, bool focused)
    -> AdjustmentTransferNodeListModel::Row {
  return AdjustmentTransferNodeListModel::Row{
      node.descriptor.node_id,
      QString::fromStdString(node.descriptor.display_name),
      static_cast<int>(node.descriptor.kind),
      node.descriptor.is_default_grade,
      NodeCheckState(node),
      focused,
  };
}

}  // namespace alcedo::ui
