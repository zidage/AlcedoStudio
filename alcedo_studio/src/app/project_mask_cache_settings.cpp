//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/project_mask_cache_settings.hpp"

#include <stdexcept>
#include <utility>

#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

constexpr auto kMetadataKey     = "mask_cache";
constexpr auto kRootPathKey     = "root_path";
constexpr auto kRetentionKey    = "retention";
constexpr auto kRevisionKey     = "settings_revision";
constexpr auto kPreviousRootsKey = "previous_roots";
constexpr auto kClosePendingKey = "close_cleanup_pending";
constexpr auto kKeepToken       = "keep";
constexpr auto kDeleteOnCloseToken = "delete_on_project_close";

auto PathToStored(const std::filesystem::path& path) -> std::string {
  return conv::ToBytes(path.wstring());
}

auto StoredToPath(const std::string& stored) -> std::filesystem::path {
  return std::filesystem::path(conv::FromBytes(stored));
}

auto RetentionToken(ProjectMaskCacheRetention retention) -> std::string_view {
  switch (retention) {
    case ProjectMaskCacheRetention::Keep:
      return kKeepToken;
    case ProjectMaskCacheRetention::DeleteOnProjectClose:
      return kDeleteOnCloseToken;
  }
  throw std::invalid_argument("Unknown project Mask cache retention");
}

auto RetentionFromToken(std::string_view token) -> ProjectMaskCacheRetention {
  if (token == kKeepToken) {
    return ProjectMaskCacheRetention::Keep;
  }
  if (token == kDeleteOnCloseToken) {
    return ProjectMaskCacheRetention::DeleteOnProjectClose;
  }
  throw std::invalid_argument("Unknown project Mask cache retention '" + std::string(token) + "'");
}

auto MetadataParent(const std::filesystem::path& meta_path) -> std::filesystem::path {
  auto parent = meta_path.parent_path();
  if (parent.empty()) {
    return std::filesystem::current_path();
  }
  return parent;
}

auto StoreChosenRoot(const std::filesystem::path& chosen_root,
                     const std::filesystem::path& meta_path) -> std::string {
  if (chosen_root.empty()) {
    return {};
  }
  const auto default_root = DefaultProjectMaskCacheChosenRoot(meta_path);
  std::error_code error;
  const auto      absolute_root = std::filesystem::absolute(chosen_root, error);
  if (error) {
    return PathToStored(chosen_root);
  }
  const auto normalized = absolute_root.lexically_normal();
  if (normalized == std::filesystem::absolute(default_root).lexically_normal()) {
    return {};
  }
  const auto relative = normalized.lexically_relative(
      std::filesystem::absolute(default_root).lexically_normal());
  const auto relative_text = relative.generic_wstring();
  if (relative.empty() || relative.has_root_name() || relative.has_root_directory() ||
      relative_text.starts_with(L"..")) {
    return PathToStored(normalized);
  }
  return PathToStored(relative);
}

}  // namespace

auto DefaultProjectMaskCacheChosenRoot(const std::filesystem::path& meta_path)
    -> std::filesystem::path {
  return MetadataParent(meta_path);
}

auto ResolveProjectMaskCacheChosenRoot(const ProjectMaskCacheSettings& settings,
                                       const std::filesystem::path& meta_path)
    -> std::filesystem::path {
  if (settings.chosen_root.empty()) {
    return DefaultProjectMaskCacheChosenRoot(meta_path);
  }
  if (settings.chosen_root.is_absolute()) {
    return settings.chosen_root.lexically_normal();
  }
  return (DefaultProjectMaskCacheChosenRoot(meta_path) / settings.chosen_root).lexically_normal();
}

auto ProjectMaskCacheNamespaceDirectory(const std::filesystem::path& chosen_root,
                                        std::string_view project_uuid) -> std::filesystem::path {
  if (chosen_root.empty()) {
    throw std::invalid_argument("Project Mask cache root must not be empty");
  }
  if (project_uuid.empty() || project_uuid.find('/') != std::string_view::npos ||
      project_uuid.find('\\') != std::string_view::npos || project_uuid == "." ||
      project_uuid == ".." || project_uuid.find("..") != std::string_view::npos) {
    throw std::invalid_argument("Project Mask cache UUID is not a safe directory name");
  }
  return chosen_root / kProjectMaskCacheDirectoryName / std::filesystem::path(project_uuid);
}

void ApplyProjectMaskCacheSettingsToMetadata(nlohmann::json& metadata,
                                             const ProjectMaskCacheSettings& settings,
                                             const std::filesystem::path& meta_path) {
  nlohmann::json body;
  body[kRootPathKey]      = StoreChosenRoot(settings.chosen_root, meta_path);
  body[kRetentionKey]     = std::string(RetentionToken(settings.retention));
  body[kRevisionKey]      = settings.settings_revision;
  body[kClosePendingKey]  = settings.close_cleanup_pending;
  nlohmann::json previous = nlohmann::json::array();
  for (const auto& root : settings.previous_roots) {
    previous.push_back(PathToStored(root));
  }
  body[kPreviousRootsKey] = std::move(previous);
  metadata[kMetadataKey]  = std::move(body);
}

auto ReadProjectMaskCacheSettingsFromMetadata(const nlohmann::json& metadata,
                                              const std::filesystem::path& meta_path)
    -> ProjectMaskCacheSettings {
  ProjectMaskCacheSettings settings;
  if (!metadata.contains(kMetadataKey) || !metadata.at(kMetadataKey).is_object()) {
    settings.chosen_root = DefaultProjectMaskCacheChosenRoot(meta_path);
    return settings;
  }
  const auto& body = metadata.at(kMetadataKey);
  if (body.contains(kRootPathKey) && body.at(kRootPathKey).is_string()) {
    const auto stored = StoredToPath(body.at(kRootPathKey).get<std::string>());
    if (stored.empty()) {
      settings.chosen_root = DefaultProjectMaskCacheChosenRoot(meta_path);
    } else if (stored.is_absolute()) {
      settings.chosen_root = stored;
    } else {
      settings.chosen_root = stored;
    }
  } else {
    settings.chosen_root = DefaultProjectMaskCacheChosenRoot(meta_path);
  }
  if (body.contains(kRetentionKey) && body.at(kRetentionKey).is_string()) {
    settings.retention = RetentionFromToken(body.at(kRetentionKey).get<std::string>());
  }
  if (body.contains(kRevisionKey) && body.at(kRevisionKey).is_number_unsigned()) {
    settings.settings_revision = body.at(kRevisionKey).get<std::uint64_t>();
  }
  if (body.contains(kClosePendingKey) && body.at(kClosePendingKey).is_boolean()) {
    settings.close_cleanup_pending = body.at(kClosePendingKey).get<bool>();
  }
  if (body.contains(kPreviousRootsKey) && body.at(kPreviousRootsKey).is_array()) {
    for (const auto& entry : body.at(kPreviousRootsKey)) {
      if (!entry.is_string()) {
        continue;
      }
      auto path = StoredToPath(entry.get<std::string>());
      if (!path.empty()) {
        settings.previous_roots.push_back(std::move(path));
      }
    }
  }
  return settings;
}

}  // namespace alcedo
