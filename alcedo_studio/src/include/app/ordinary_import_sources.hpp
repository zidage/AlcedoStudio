//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "sleeve/storage.hpp"
#include "type/source_file_record.hpp"
#include "type/type.hpp"

namespace alcedo {

/// The source measurements and path lookup for one ordinary import. The submission thread owns
/// the lookup. Workers publish measurements only after writing a file's history root. At the end
/// of the job, the measurements pass to SyncImports for storage with the finished import.
class OrdinaryImportSources {
 public:
  explicit OrdinaryImportSources(Storage& storage) : storage_(storage) {}

  struct Candidates {
    std::vector<sl_element_id_t> stored;
    std::vector<sl_element_id_t> submitted;
  };

  struct Match {
    sl_element_id_t element_id;
    bool            needs_measurement;
  };

  /// Start duplicate checking. A failed catalog read logs the error and leaves checking disabled.
  void LoadCandidates();
  /// Empty when checking is disabled or this path cannot be normalized.
  auto TrackPath(const image_path_t& path) -> Candidates*;
  /// Find a match, or an older entry whose source has never been measured. Reads only.
  auto FindMatch(const Candidates& candidates, const SourceFileRecord& source) const
      -> std::optional<Match>;
  /// An unavailable measurement is logged and leaves the photo eligible for import.
  static auto Measure(const image_path_t& path, sl_element_id_t element_id = 0)
      -> std::optional<SourceFileRecord>;

  void RecordSource(SourceFileRecord record);
  auto TakeSourceRecords() -> std::vector<SourceFileRecord>;

 private:
  auto CompareMeasurement(sl_element_id_t id, const SourceFileRecord& source) const
      -> std::optional<bool>;

  Storage&                                                                          storage_;
  std::optional<std::unordered_map<std::filesystem::path::string_type, Candidates>> candidates_;
  mutable std::mutex                                                                mutex_;
  std::unordered_map<sl_element_id_t, SourceFileRecord>                             records_;
};

}  // namespace alcedo
