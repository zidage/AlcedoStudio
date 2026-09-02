//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mask_asset_reachability.hpp"

#include "app/pipeline_history_applier.hpp"

#include <cctype>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace alcedo {
namespace {

auto HexValue(char digit) -> int {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

auto KeyFromSafeFileName(std::string_view file_name) -> std::optional<MaskAssetKey> {
  constexpr std::string_view kSuffix = ".r8mask";
  if (file_name.size() <= kSuffix.size() ||
      file_name.substr(file_name.size() - kSuffix.size()) != kSuffix) {
    return std::nullopt;
  }
  const auto stem = file_name.substr(0, file_name.size() - kSuffix.size());
  if (stem.size() % 2 != 0 || stem.empty()) {
    return std::nullopt;
  }
  std::string key;
  key.reserve(stem.size() / 2);
  for (std::size_t index = 0; index < stem.size(); index += 2) {
    const int high = HexValue(stem[index]);
    const int low  = HexValue(stem[index + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    key.push_back(static_cast<char>((high << 4) | low));
  }
  if (key.empty()) {
    return std::nullopt;
  }
  return MaskAssetKey{std::move(key)};
}

void InsertAssetKey(const nlohmann::json& source, std::set<MaskAssetKey>* keys) {
  if (!source.is_object() || !source.contains("asset_key") || source.at("asset_key").is_null() ||
      !source.at("asset_key").is_string()) {
    return;
  }
  const auto text = source.at("asset_key").get<std::string>();
  if (!text.empty()) {
    keys->insert(MaskAssetKey{text});
  }
}

void InsertKeysFromMaskJson(const nlohmann::json& mask, std::set<MaskAssetKey>* keys) {
  if (!mask.is_object() || !mask.contains("source")) {
    return;
  }
  InsertAssetKey(mask.at("source"), keys);
}

void InsertKeysFromGradeJson(const nlohmann::json& grade, std::set<MaskAssetKey>* keys) {
  if (!grade.is_object() || !grade.contains("masks") || !grade.at("masks").is_array()) {
    return;
  }
  for (const auto& mask : grade.at("masks")) {
    InsertKeysFromMaskJson(mask, keys);
  }
}

void InsertKeysFromChange(const PipelineEditChange& change, std::set<MaskAssetKey>* keys) {
  std::visit(
      [&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, AddColorGradeChange> ||
                      std::is_same_v<T, RemoveColorGradeChange>) {
          InsertKeysFromGradeJson(typed.node, keys);
        } else if constexpr (std::is_same_v<T, AddMaskChange> ||
                             std::is_same_v<T, RemoveMaskChange>) {
          InsertKeysFromMaskJson(typed.mask, keys);
        } else if constexpr (std::is_same_v<T, ReplaceMaskSourceChange> ||
                             std::is_same_v<T, ReplaceMaskAssetChange>) {
          InsertAssetKey(typed.before_source, keys);
          InsertAssetKey(typed.after_source, keys);
        }
      },
      change);
}

}  // namespace

auto CollectMaskAssetKeysFromBatch(const PipelineEditBatch& batch) -> std::set<MaskAssetKey> {
  std::set<MaskAssetKey> keys;
  for (const auto& change : batch.changes) {
    InsertKeysFromChange(change, &keys);
  }
  return keys;
}

auto CollectMaskAssetKeysFromCommit(const EditCommit& commit) -> std::set<MaskAssetKey> {
  if (!IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    return {};
  }
  try {
    return CollectMaskAssetKeysFromBatch(PipelineEditBatch::FromJSON(commit.GetPayloadJSON()));
  } catch (const std::exception&) {
    return {};
  }
}

auto CollectReachableMaskAssetKeys(const MaskAssetReachabilityScan& scan)
    -> std::set<MaskAssetKey> {
  std::set<MaskAssetKey> keys;
  for (const auto* document : scan.documents) {
    if (document == nullptr) {
      continue;
    }
    for (const auto& key : CollectPersistentMaskAssetKeys(*document)) {
      keys.insert(key);
    }
  }
  for (const auto* batch : scan.batches) {
    if (batch == nullptr) {
      continue;
    }
    const auto from_batch = CollectMaskAssetKeysFromBatch(*batch);
    keys.insert(from_batch.begin(), from_batch.end());
  }
  for (const auto* commit : scan.commits) {
    if (commit == nullptr) {
      continue;
    }
    const auto from_commit = CollectMaskAssetKeysFromCommit(*commit);
    keys.insert(from_commit.begin(), from_commit.end());
  }
  for (const auto* record : scan.wal_records) {
    if (record == nullptr || !record->edit_commit.has_value()) {
      continue;
    }
    const auto from_wal = CollectMaskAssetKeysFromCommit(*record->edit_commit);
    keys.insert(from_wal.begin(), from_wal.end());
  }
  keys.insert(scan.extra_keys.begin(), scan.extra_keys.end());
  return keys;
}

auto DeleteUnreachableMaskAssetFiles(MaskStore& store, const std::set<MaskAssetKey>& reachable)
    -> MaskAssetMaintenanceReport {
  MaskAssetMaintenanceReport report;
  std::error_code            list_error;
  if (!std::filesystem::exists(store.Root(), list_error)) {
    return report;
  }
  if (!std::filesystem::is_directory(store.Root(), list_error)) {
    report.failures.push_back("Mask store root is not a directory: " + store.Root().string());
    return report;
  }
  for (const auto& entry : std::filesystem::directory_iterator(store.Root(), list_error)) {
    if (list_error) {
      report.failures.push_back("Failed to list Mask store: " + list_error.message());
      break;
    }
    std::error_code file_error;
    if (!entry.is_regular_file(file_error) || file_error) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (name.find(".tmp.") != std::string::npos) {
      continue;
    }
    const auto decoded = KeyFromSafeFileName(name);
    if (!decoded.has_value()) {
      continue;
    }
    const auto exact_path = store.PathFor(*decoded);
    if (std::filesystem::equivalent(exact_path, entry.path(), file_error) == false || file_error) {
      if (exact_path != entry.path()) {
        continue;
      }
    }
    try {
      const auto loaded = store.Load(*decoded);
      if (!loaded) {
        report.failures.push_back("Mask asset file failed validation: " + entry.path().string());
        continue;
      }
    } catch (const std::exception& ex) {
      report.failures.push_back(std::string{"Mask asset file is corrupt: "} + entry.path().string() +
                                ": " + ex.what());
      continue;
    }
    if (reachable.contains(*decoded)) {
      continue;
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(exact_path, remove_error) || remove_error) {
      report.failures.push_back("Failed to remove unreferenced Mask asset: " + exact_path.string() +
                                (remove_error ? (": " + remove_error.message()) : ""));
      continue;
    }
    report.removed_paths.push_back(exact_path);
  }
  return report;
}

}  // namespace alcedo
