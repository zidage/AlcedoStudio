//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <string_view>

#include "app/editor_node_graph_draft.hpp"

namespace alcedo::ui {

/**
 * @brief Translate one known Nodes-page draft admission issue for display.
 *
 * Domain validation stays in EditorNodeGraphDraft. This helper is the single
 * application presentation boundary for those known issues. Unknown failures
 * keep @p technical_detail unchanged so WAL, adapter, and QML errors remain
 * exact.
 *
 * @param issue Domain admission discriminator. @c None returns @p technical_detail.
 * @param technical_detail Exact domain or infrastructure text. Parameterized
 *        issues use this as the NodeId (or other) argument; unknown issues use
 *        it as the full message.
 * @return Localized user-facing text on the GUI thread. Never empty when
 *        @p technical_detail is non-empty and @p issue is @c None.
 * @note Synchronous. Does not read the live graph, session, or Qan adapter.
 */
[[nodiscard]] auto PresentNodeGraphDraftIssue(NodeGraphDraftIssue issue,
                                              std::string_view    technical_detail) -> QString;

/**
 * @brief Present a draft mutation error, or the raw technical text when unknown.
 */
[[nodiscard]] inline auto PresentNodeGraphDraftMutation(
    const EditorNodeGraphDraftMutation& mutation) -> QString {
  return PresentNodeGraphDraftIssue(mutation.issue, mutation.error);
}

}  // namespace alcedo::ui
