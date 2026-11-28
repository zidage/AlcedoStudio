// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/image/source_file_store.hpp"
#include "type/type.hpp"

namespace alcedo {
/// Derived preservation evidence only. No Image, graph, document, or asset is retained.
struct MaintenanceEntryEvidence {
  sl_element_id_t          entry_id = 0;
  image_path_t             source_path;
  std::optional<int64_t>   added_time;
  int                      rating           = 0;
  size_t                   commit_count     = 0;
  size_t                   version_count    = 0;
  bool                     valid_references = true;
  std::vector<std::string> protection_reasons;
  /// Minimal evidence carried through the live source check. The store validates it on write.
  std::optional<SourceFileFingerprint> recorded_fingerprint;
  bool                                 source_changed = false;
  bool                                 mounted        = false;
};

/// An explicit removal and, for a duplicate, its retained project entry.
struct MaintenanceRemoval {
  sl_element_id_t                entry_id = 0;
  std::optional<sl_element_id_t> retained_id;
};

/// Scoped maintenance reads and atomic removal of pristine entries in an isolated database.
/// Connections and the database lock live for one operation. Never attach this mutator to UI.
class ProjectMaintenanceStore {
 public:
  explicit ProjectMaintenanceStore(Database& database) : database_(database) {}
  /// Up to 64 ordinary file IDs after @p after, including broken file bindings. Read only.
  auto ListEntryIds(sl_element_id_t after) -> std::vector<sl_element_id_t>;
  /// Bounded orphan Image identifiers/paths. They have no removable project-entry ID.
  auto ListUnboundImages(image_id_t after) -> std::vector<std::pair<image_id_t, image_path_t>>;
  /// Inspect all stored Versions and commits. Invalid evidence protects the entry.
  auto Inspect(sl_element_id_t id) -> MaintenanceEntryEvidence;
  /// Publish completed ordinary-source checks. Returns true if a saved mark changed.
  auto UpdateSourceChangeMarks(std::span<const SourceChangeMark> changes) -> bool;
  /// Validate the entire explicit batch before one transaction. Caller has freshly checked
  /// source statuses. Throws with rollback on any invalid entry, pair, or owner failure.
  /// Progress runs on the caller's thread, under the transaction lock, before commit.
  /// It reports processed entries, not a durable removal result. Callback failure rolls back.
  auto RemovePristineProjectEntries(std::span<const MaintenanceRemoval>        removals,
                                    const std::function<void(size_t, size_t)>& progress = {})
      -> std::vector<sl_element_id_t>;
  /// Hash expected retained rows through DuckDB, including the membership union. Only digests
  /// leave the owner. Before removal derives Image IDs; after removal uses that scalar list.
  auto RetainedRowDigests(std::span<const MaintenanceRemoval> removals,
                          std::span<const image_id_t> removed_images, bool before_removal)
      -> std::map<std::string, std::string>;
  /// Minimal Image IDs of the explicit entries, for last-reference verification.
  auto ImageIds(std::span<const MaintenanceRemoval> removals) -> std::vector<image_id_t>;

 private:
  Database& database_;
};
}  // namespace alcedo
