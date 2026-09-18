//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_graph_presentation.hpp"

#include <QCoreApplication>
#include <QString>

namespace alcedo::ui {
namespace {

auto Translate(const char* source) -> QString {
  return QCoreApplication::translate("EditorNodeGraphPresentation", source);
}

auto DetailText(std::string_view detail) -> QString {
  return QString::fromUtf8(detail.data(), static_cast<qsizetype>(detail.size()));
}

}  // namespace

auto PresentNodeGraphDraftIssue(NodeGraphDraftIssue issue, std::string_view technical_detail)
    -> QString {
  switch (issue) {
    case NodeGraphDraftIssue::DuplicateColorGradeIdentity:
      return Translate("A Color Grade with that identity already exists");
    case NodeGraphDraftIssue::NodeNotInGraph:
      return Translate("That node is not in the current graph");
    case NodeGraphDraftIssue::OnlyColorGradeCanBeDeleted:
      return Translate("Only a Color Grade can be deleted");
    case NodeGraphDraftIssue::SelfConnection:
      return Translate("A node cannot connect to itself");
    case NodeGraphDraftIssue::DevelopHasNoIncomingPort:
      return Translate("Develop has no incoming image port");
    case NodeGraphDraftIssue::DrtHasNoOutgoingPort:
      return Translate("DRT/Post has no outgoing image port");
    case NodeGraphDraftIssue::UnsupportedSourceType:
      return Translate("Unsupported source node type: %1").arg(DetailText(technical_detail));
    case NodeGraphDraftIssue::UnsupportedDestinationType:
      return Translate("Unsupported destination node type: %1").arg(DetailText(technical_detail));
    case NodeGraphDraftIssue::Cycle:
      return Translate("That connection would create a cycle");
    case NodeGraphDraftIssue::None:
      return DetailText(technical_detail);
  }
  return DetailText(technical_detail);
}

}  // namespace alcedo::ui
