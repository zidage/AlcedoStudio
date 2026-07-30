//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"

namespace alcedo::ui {

/// User-facing presentation of one history commit row. Produced by a pure
/// helper from the stable field key and the semantic before/after payload; QML
/// renders these strings directly and never parses commit JSON.
struct EditorHistoryCommitPresentation {
  /// Capitalized adjustment name (e.g. "Exposure"); "Merge" for merge commits.
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
  /// True for merge commits; before_text/after_text are empty and merge_summary
  /// carries the compact provenance line.
  bool is_merge = false;
  /// Compact merge provenance (e.g. "Resolved 3 fields"). Empty for edits.
  QString merge_summary;
};

/// Pure payload-to-display conversion for one editor history commit. Maps the
/// stable field key and the OrdinaryEditPayload before/after JSON to
/// user-facing text and an operator glyph key. Has no dependency on the editor
/// session, commit graph, pipeline, or QML engine, so it is unit-testable in
/// isolation.
///
/// `field_key` is the stable editor adjustment key (e.g. "exposure") or
/// "merge"/empty for merge commits. `before_value_json`/`after_value_json` are
/// the serialized operator params captured before and after the settled edit;
/// empty strings are treated as empty objects. `kind` selects the merge path.
/// `merge_field_keys` carries the ordered resolved fields of a merge commit and
/// is ignored for ordinary edits.
auto PresentEditorHistoryCommit(const std::string&              field_key,
                                const std::string&              before_value_json,
                                const std::string&              after_value_json,
                                bool                            before_enabled,
                                bool                            after_enabled,
                                EditCommitKind                  kind,
                                const std::vector<std::string>& merge_field_keys = {})
    -> EditorHistoryCommitPresentation;

}  // namespace alcedo::ui