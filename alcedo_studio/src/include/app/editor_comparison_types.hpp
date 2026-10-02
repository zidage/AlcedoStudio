//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>

#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_types.hpp"

/**
 * @file
 * @brief Sides of the editor comparison: two states of the open image, both rendered with the
 *        current sensor settings. EditorComparisonInputs builds them.
 */
namespace alcedo {

/// Which state of the open image one side of a comparison shows.
enum class EditorComparisonSourceKind : std::uint8_t {
  /// The imported root: the image before any history commit.
  Root,
  /// The working values captured when the comparison opened, including uncommitted values.
  Current,
  /// The saved head of one named Version. The active Version shows the captured working values.
  Version,
};

/// Selection of one comparison side.
struct EditorComparisonSource {
  EditorComparisonSourceKind kind = EditorComparisonSourceKind::Current;
  /// Version identity when @ref kind is Version; ignored otherwise.
  version_ref_id_t           version_id{};

  [[nodiscard]] static auto  Root() -> EditorComparisonSource {
    return {EditorComparisonSourceKind::Root, {}};
  }
  [[nodiscard]] static auto Current() -> EditorComparisonSource {
    return {EditorComparisonSourceKind::Current, {}};
  }
  [[nodiscard]] static auto Version(version_ref_id_t id) -> EditorComparisonSource {
    return {EditorComparisonSourceKind::Version, id};
  }
};

/**
 * @brief One side of a comparison: an immutable document and where it came from.
 *
 * Purpose: the editor comparison renders two fixed states later, on the editor executor, while
 * the working document stays live; neither state may change in between. @ref snapshot is a
 * preview of the current editor lineage and image. For the current working values it is the
 * captured preview itself. For any other state it is a private replay frozen after
 * PipelineDocument::UseSensorSettingsFrom, so it shares the current sensor settings and their
 * stamps but keeps its own white balance, geometry, Color Grades, Masks, and DRT. It is never a
 * committed snapshot, because its sensor settings can differ from the stored head.
 *
 * Owner: the comparison that requested it. Released with the pair on replacement or close.
 */
struct EditorComparisonInput {
  /// Provenance for labels; not a history identity of @ref snapshot.
  EditorComparisonSource                       source;
  /// History head that was replayed. Empty for Root and for the captured working values.
  head_commit_hash_t                           source_head;
  std::shared_ptr<const PipelineGraphSnapshot> snapshot;
};

/// Both sides of one comparison. Built together; never published partly.
struct EditorComparisonInputPair {
  EditorComparisonInput a;
  EditorComparisonInput b;
};

}  // namespace alcedo
