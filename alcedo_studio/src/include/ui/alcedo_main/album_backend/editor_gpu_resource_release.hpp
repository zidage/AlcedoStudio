//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace alcedo::ui {

/**
 * @brief Work that renders on its own GPU allocations while the editor keeps its image open.
 *
 * Each holder asks the editor to release its GPU resources before that work allocates. The
 * editor renders again only when no holder holds the release.
 */
enum class EditorGpuReleaseHolder : std::size_t {
  Export             = 0,
  SemanticGeneration = 1,
};

inline constexpr std::size_t kEditorGpuReleaseHolderCount = 2;

/// Log and message name of @p holder: "export" or "semantic generation".
constexpr auto EditorGpuReleaseHolderName(EditorGpuReleaseHolder holder) -> std::string_view {
  switch (holder) {
    case EditorGpuReleaseHolder::Export:
      return "export";
    case EditorGpuReleaseHolder::SemanticGeneration:
      return "semantic generation";
  }
  return "unknown";
}

/**
 * @brief Return path of the editor GPU resources for one holder.
 *
 * `release` runs on the GUI thread before the holder allocates. It calls its completion once, on
 * any thread, with the real error when the release failed. `resume` runs on the GUI thread once
 * when the work of that holder ends, also after a failed release. The editor renders again after
 * the `resume` of the last holder.
 */
struct EditorGpuResourceReleaseHooks {
  using ReleaseCompletion = std::function<void(bool success, std::string error)>;
  std::function<void(ReleaseCompletion)> release;
  std::function<void()>                  resume;
};

}  // namespace alcedo::ui
