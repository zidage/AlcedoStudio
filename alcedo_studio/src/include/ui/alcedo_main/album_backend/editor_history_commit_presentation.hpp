//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"

namespace alcedo {
struct EditorHistoryCommit;
}

namespace alcedo::ui {

/// User-facing presentation of one history commit row. Produced by a pure
/// helper from the stable field key and the semantic before/after payload; QML
/// renders these strings directly and never parses commit JSON.
struct EditorHistoryCommitPresentation {
  /// Capitalized adjustment name (e.g. "Exposure").
  QString display_name;
  /// Formatted previous value (e.g. "0.00", "Rec.709", "Auto"). Empty when the
  /// commit has no meaningful prior value (first application of a field).
  QString before_text;
  /// Formatted current value (e.g. "+0.35", "Display P3", "Cleared").
  QString after_text;
  /// One-line contextual delta (e.g. "0.00 \u2192 +0.35", "Set to +0.35",
  /// "Red Hue +5\u00b0"). Used for tooltips and accessible descriptions.
  QString delta_text;
  /// qrc resource path of the operator glyph (e.g. ":/history_icons/sun-medium.svg").
  QString icon_key;
};

/// Present an adjustment-field commit from its stored field key and JSON.
///
/// Prefer @ref PresentEditorHistoryCommit(const alcedo::EditorHistoryCommit&)
/// when the row came from a typed pipeline batch so localization keys and
/// saved node names are used.
auto PresentEditorHistoryCommit(const std::string& field_key,
                                const std::string& before_value_json,
                                const std::string& after_value_json,
                                bool               before_enabled,
                                bool               after_enabled)
    -> EditorHistoryCommitPresentation;

/**
 * @brief Present one published history row, including typed graph operations.
 *
 * @param commit Immutable history projection row. Graph Add/Rename/Delete rows
 *        use @c presentation_key and saved @c node_display_name. Adjustment
 *        rows still use @c field_key and before/after JSON.
 * @return Display strings and glyph key. Never throws; unknown keys fall back
 *         to the adjustment formatter.
 * @note Pure and synchronous. Does not read live selection or the document.
 */
auto PresentEditorHistoryCommit(const alcedo::EditorHistoryCommit& commit)
    -> EditorHistoryCommitPresentation;

}  // namespace alcedo::ui