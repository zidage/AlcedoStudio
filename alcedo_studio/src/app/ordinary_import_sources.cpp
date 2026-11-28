//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ordinary_import_sources.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "utils/file/source_file.hpp"
#include "utils/file/source_file_fingerprint.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

auto IsSameSource(const SourceFileRecord& existing, const SourceFileRecord& incoming) -> bool {
  return existing.state == SourceFileState::kPresent && existing.byte_size == incoming.byte_size &&
         existing.modified_ns == incoming.modified_ns &&
         existing.fingerprint == incoming.fingerprint &&
         existing.fingerprint_version == incoming.fingerprint_version;
}

auto ReadMeasurement(const image_path_t& path, sl_element_id_t element_id) -> SourceFileRecord {
  const auto attributes = ReadSourceFileAttributes(path);
  if (!attributes) {
    throw std::runtime_error("cannot read source attributes");
  }
  const auto fingerprint = ComputeSourceFileFingerprint(path);
  switch (fingerprint.error) {
    case SourceFileFingerprintError::kCannotOpen:
      throw std::runtime_error("cannot open source for fingerprinting");
    case SourceFileFingerprintError::kReadFailed:
      throw std::runtime_error("cannot read source for fingerprinting");
    case SourceFileFingerprintError::kSizeChangedDuringRead:
      throw std::runtime_error("source size changed during fingerprinting");
    case SourceFileFingerprintError::kNone:
      break;
  }
  if (attributes != ReadSourceFileAttributes(path)) {
    throw std::runtime_error("source attributes changed during fingerprinting");
  }
  return SourceFileRecord{.element_id          = element_id,
                          .byte_size           = attributes->byte_size,
                          .modified_ns         = attributes->modified_ns,
                          .fingerprint         = fingerprint.fingerprint.hash,
                          .fingerprint_version = fingerprint.fingerprint.version};
}

}  // namespace

void OrdinaryImportSources::LoadCandidates() {
  try {
    const auto files = storage_.OpenSession().ReadImportedFileLocations();
    candidates_.emplace();
    for (const auto& file : files) {
      if (!file.image_path.empty()) {
        (*candidates_)[SourcePathKey(file.image_path)].stored.push_back(file.element_id);
      }
    }
  } catch (const std::exception& error) {
    candidates_.reset();
    ALCEDO_LOG_WARN(Library) << "Import duplicate checking disabled: " << error.what();
  }
}

auto OrdinaryImportSources::LookupPath(const image_path_t& path) const
    -> std::optional<PathLookup> {
  if (!candidates_) {
    return std::nullopt;
  }
  try {
    auto       key    = SourcePathKey(path);
    const auto found  = candidates_->find(key);
    auto       status = CandidateStatus::kNone;
    if (found != candidates_->end()) {
      status =
          found->second.submitted.empty() ? CandidateStatus::kStored : CandidateStatus::kSubmitted;
    }
    return PathLookup{std::move(key), status};
  } catch (const std::exception& error) {
    ALCEDO_LOG_WARN(Library) << "Import duplicate check for " << path << ": " << error.what();
    return std::nullopt;
  }
}

void OrdinaryImportSources::RecordSubmission(std::filesystem::path::string_type key,
                                             sl_element_id_t                    element_id) {
  (*candidates_)[std::move(key)].submitted.push_back(element_id);
}

auto OrdinaryImportSources::Measure(const image_path_t& path, sl_element_id_t element_id)
    -> std::optional<SourceFileRecord> {
  try {
    return ReadMeasurement(path, element_id);
  } catch (const std::exception& error) {
    ALCEDO_LOG_WARN(Library) << "Import source measurement for " << path << ": " << error.what();
    return std::nullopt;
  }
}

auto OrdinaryImportSources::FindMatch(const std::filesystem::path::string_type& key,
                                      const SourceFileRecord&                   source) const
    -> std::optional<Match> {
  const auto& candidates = candidates_->at(key);
  for (const auto id : candidates.submitted) {
    if (CompareMeasurement(id, source).value_or(false)) {
      return Match{id, false};
    }
  }
  sl_element_id_t unmeasured = 0;
  bool            has_record = false;
  auto&           store      = storage_.GetSourceFileStore();
  for (const auto id : candidates.stored) {
    auto matches = CompareMeasurement(id, source);
    if (!matches) {
      if (const auto record = store.ReadSourceFile(id)) {
        matches = IsSameSource(*record, source);
      }
    }
    if (matches) {
      has_record = true;
      if (*matches) {
        return Match{id, false};
      }
    } else if (unmeasured == 0) {
      unmeasured = id;
    }
  }
  if (unmeasured != 0 && !has_record) {
    return Match{unmeasured, true};
  }
  return std::nullopt;
}

auto OrdinaryImportSources::CompareMeasurement(sl_element_id_t         id,
                                               const SourceFileRecord& source) const
    -> std::optional<bool> {
  std::lock_guard lock(mutex_);
  const auto      it = records_.find(id);
  return it == records_.end() ? std::nullopt : std::optional{IsSameSource(it->second, source)};
}

void OrdinaryImportSources::RecordSource(SourceFileRecord record) {
  std::lock_guard lock(mutex_);
  records_.insert_or_assign(record.element_id, std::move(record));
}

auto OrdinaryImportSources::TakeSourceRecords() -> std::vector<SourceFileRecord> {
  std::lock_guard               lock(mutex_);
  std::vector<SourceFileRecord> records;
  records.reserve(records_.size());
  for (auto& [id, record] : records_) {
    records.push_back(std::move(record));
  }
  records_.clear();
  return records;
}

}  // namespace alcedo
