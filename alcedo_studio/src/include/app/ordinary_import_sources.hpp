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

  enum class CandidateStatus { kNone, kStored, kSubmitted };

  struct PathLookup {
    std::filesystem::path::string_type key;
    CandidateStatus                    status;
  };

  struct Match {
    sl_element_id_t element_id;
    bool            needs_source_record;
  };

  /// Start duplicate checking. A failed catalog read logs the error and leaves checking disabled.
  void LoadCandidates();
  /// Return the normalized key and candidate status, or empty if the path cannot be checked.
  /// A submitted candidate must finish before FindMatch reads its measurement.
  auto        LookupPath(const image_path_t& path) const -> std::optional<PathLookup>;
  /// Record a file submitted for a path returned by LookupPath. Runs on the submission thread.
  void        RecordSubmission(std::filesystem::path::string_type key, sl_element_id_t element_id);
  /// Find a match, or an older entry that needs a source record. Reads only.
  /// Use a key with stored or submitted candidates from LookupPath.
  auto        FindMatch(const std::filesystem::path::string_type& key,
                        const SourceFileRecord&                   source) const -> std::optional<Match>;
  /// An unavailable measurement is logged and leaves the photo eligible for import.
  static auto Measure(const image_path_t& path, sl_element_id_t element_id = 0)
      -> std::optional<SourceFileRecord>;

  /// Retain a measurement for an existing or newly committed library entry.
  void RecordSource(SourceFileRecord record);
  /// Transfer the measurements for saving after all workers have finished.
  auto TakeSourceRecords() -> std::vector<SourceFileRecord>;

 private:
  struct Candidates {
    std::vector<sl_element_id_t> stored;
    std::vector<sl_element_id_t> submitted;
  };

  auto CompareMeasurement(sl_element_id_t id, const SourceFileRecord& source) const
      -> std::optional<bool>;

  Storage&                                                                          storage_;
  std::optional<std::unordered_map<std::filesystem::path::string_type, Candidates>> candidates_;
  mutable std::mutex                                                                mutex_;
  std::unordered_map<sl_element_id_t, SourceFileRecord>                             records_;
};

}  // namespace alcedo
