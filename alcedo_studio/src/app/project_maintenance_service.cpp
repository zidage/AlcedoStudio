// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "app/project_maintenance_service.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>

#include "utils/file/source_file.hpp"

namespace alcedo {
auto ProjectMaintenanceService::CheckSource(const image_path_t& path) -> MaintenanceSourceCheck {
  return CheckSourceImageStatus(path);
}

void ProjectMaintenanceService::Clear() {
  complete_ = false;
  changed_marks_written_ = false;
  findings_.clear();
}

auto ProjectMaintenanceService::Scan(ProjectMaintenanceStore&           store,
                                     const std::filesystem::path&       journal_directory,
                                     std::optional<sl_element_id_t>     active_entry,
                                     const std::function<bool()>&       canceled,
                                     const std::function<void(size_t)>& progress,
                                     SourceChecker checker, bool mark_changed_sources) -> bool {
  Clear();
  if (!checker) checker = CheckSource;
  std::map<image_path_t::string_type, std::vector<MaintenanceEntryEvidence>> groups;
  sl_element_id_t                                               after     = 0;
  size_t                                                        inspected = 0;
  while (true) {
    if (canceled && canceled()) return false;
    const auto ids = store.ListEntryIds(after);
    if (ids.empty()) break;
    for (auto id : ids) {
      if (canceled && canceled()) return false;
      auto evidence = store.Inspect(id);
      after         = id;
      if (evidence.mounted) continue;
      if (active_entry == id) evidence.protection_reasons.emplace_back("Open editor entry");
      if (!journal_directory.empty()) {
        const auto journal = journal_directory / ("image-" + std::to_string(id) + ".mini-git.wal");
        std::error_code error;
        if (std::filesystem::exists(journal, error) || error) {
          evidence.protection_reasons.emplace_back("Pending or unreadable editor journal");
        }
      }
      try {
        if (!evidence.source_path.is_absolute()) {
          throw std::invalid_argument("Source image path must be absolute");
        }
        groups[SourcePathKey(evidence.source_path)].push_back(std::move(evidence));
      } catch (const std::exception& error) {
        findings_.push_back(
            {std::move(evidence), MaintenanceFindingKind::ProjectProblem, error.what(), {}, false});
      }
      if (progress) progress(++inspected);
    }
  }
  std::vector<SourceChangeMark> marks;
  for (auto& [path, entries] : groups) {
    if (canceled && canceled()) return false;
    const auto source = checker(entries.front().source_path);
    const bool repeated = entries.size() > 1;
    const bool check_fingerprint =
        repeated || std::any_of(entries.begin(), entries.end(),
                                [](const auto& entry) { return entry.source_changed; });
    const auto current = source.status == MaintenanceSourceStatus::Readable && check_fingerprint
                             ? ReadMaintenanceFingerprint(entries.front().source_path)
                             : std::nullopt;
    const bool verified_duplicates =
        repeated && current && std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
          return entry.recorded_fingerprint == current;
        });
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
      if (left.added_time != right.added_time) {
        if (!left.added_time) return false;
        if (!right.added_time) return true;
        return *left.added_time < *right.added_time;
      }
      return left.entry_id < right.entry_id;
    });
    auto survivor = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
      return entry.valid_references && !entry.protection_reasons.empty();
    });
    if (survivor == entries.end())
      survivor = std::find_if(entries.begin(), entries.end(),
                              [](const auto& entry) { return entry.valid_references; });
    const auto retained = survivor == entries.end() ? entries.front().entry_id : survivor->entry_id;
    for (auto& entry : entries) {
      const bool comparable = current && entry.recorded_fingerprint &&
                              current->version == entry.recorded_fingerprint->version;
      const bool changed    = comparable && *current != *entry.recorded_fingerprint;
      if (mark_changed_sources && comparable && changed != entry.source_changed) {
        marks.push_back({entry.entry_id, *entry.recorded_fingerprint, changed});
      }
      MaintenanceFinding finding;
      finding.reason = source.reason;
      if (!entry.valid_references) {
        finding.kind   = MaintenanceFindingKind::ProjectProblem;
        finding.reason = "Project references or history need review";
      } else if (source.status == MaintenanceSourceStatus::Missing) {
        finding.kind      = MaintenanceFindingKind::MissingSource;
        finding.removable = entry.protection_reasons.empty();
      } else if (source.status == MaintenanceSourceStatus::Error) {
        finding.kind = MaintenanceFindingKind::SourceProblem;
      } else if (verified_duplicates) {
        finding.kind        = MaintenanceFindingKind::Duplicate;
        finding.reason      = "Same path and recorded fingerprint match the current file";
        finding.retained_id = retained;
        finding.removable   = entry.protection_reasons.empty() && entry.entry_id != retained;
        if (entry.entry_id == retained) finding.reason += ". Retained entry";
      } else if (changed) {
        finding.kind   = MaintenanceFindingKind::SourceChanged;
        finding.reason = "Source changed since import";
      } else if (repeated || (entry.source_changed && !comparable)) {
        finding.kind   = MaintenanceFindingKind::SourceHistoryUnknown;
        finding.reason = comparable ? "Matches current file. Another entry needs review"
                                    : "Source history could not be verified";
      } else {
        continue;
      }
      finding.evidence = std::move(entry);
      findings_.push_back(std::move(finding));
    }
  }
  image_id_t after_image = 0;
  while (true) {
    if (canceled && canceled()) return false;
    const auto images = store.ListUnboundImages(after_image);
    if (images.empty()) break;
    for (const auto& [id, path] : images) {
      MaintenanceFinding finding;
      finding.evidence.source_path = path;
      finding.kind                 = MaintenanceFindingKind::ProjectProblem;
      finding.reason = "Image " + std::to_string(id) + " has no project-entry binding";
      finding.evidence.protection_reasons.push_back(finding.reason);
      findings_.push_back(std::move(finding));
      after_image = id;
    }
  }
  if (canceled && canceled()) return false;
  changed_marks_written_ = store.UpdateSourceChangeMarks(marks);
  complete_ = true;
  return true;
}

auto ProjectMaintenanceService::Selection(std::span<const sl_element_id_t> ids) const
    -> std::vector<MaintenanceRemoval> {
  if (!complete_ || ids.empty())
    throw std::invalid_argument("A completed scan and explicit IDs are required");
  std::unordered_set<sl_element_id_t> unique;
  std::vector<MaintenanceRemoval>     result;
  for (auto id : ids) {
    const auto finding = std::find_if(findings_.begin(), findings_.end(), [id](const auto& item) {
      return item.evidence.entry_id == id;
    });
    if (!unique.insert(id).second || finding == findings_.end() || !finding->removable) {
      throw std::invalid_argument("Selected entry is repeated, protected, or absent from the scan");
    }
    result.push_back({id, finding->retained_id});
  }
  return result;
}
}  // namespace alcedo
