//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/edit_history.hpp"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <string>

#include "edit/pipeline/pipeline_cpu.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
auto DefaultPipelineParams() -> nlohmann::json {
  CPUPipelineExecutor exec;
  return exec.ExportPipelineParams();
}

}  // namespace

VersionNode::VersionNode(Version& ver) : ver_ref_(ver) {}

EditHistory::EditHistory(sl_element_id_t bound_image) : bound_image_(bound_image) {
  SetAddTime();
  import_pipeline_params_ = DefaultPipelineParams();
  EnsureDefaultVersion();
  history_id_ = Hash128::Blend(Hash128::Compute(&added_time_, sizeof(added_time_)),
                               Hash128::Compute(&bound_image, sizeof(bound_image)));
}

void EditHistory::EnsureDefaultVersion() {
  if (version_storage_.find(default_version_id_) != version_storage_.end()) {
    return;
  }

  Version default_version                 = Version::Default(bound_image_, import_pipeline_params_);
  default_version_id_                     = default_version.GetVersionID();
  active_version_id_                      = default_version_id_;
  active_pipeline_params_                 = default_version.GetMaterializedParams();
  version_storage_[default_version_id_]   = std::move(default_version);
  version_order_.clear();
  version_order_.emplace_back(version_storage_[default_version_id_]);
  version_order_.back().commit_id_ = static_cast<p_hash_t>(version_order_.size());
}

void EditHistory::CalculateHistoryID() {
  history_id_ = Hash128::Blend(
      history_id_,
      Hash128::Blend(active_version_id_,
                     Hash128::Compute(&last_modified_time_, sizeof(last_modified_time_))));
}

void EditHistory::SetAddTime() {
  added_time_         = std::chrono::system_clock::to_time_t(TimeProvider::Now());
  last_modified_time_ = added_time_;
}

void EditHistory::SetLastModifiedTime() {
  last_modified_time_ = std::chrono::system_clock::to_time_t(TimeProvider::Now());
}

auto EditHistory::GetAddTime() const -> std::time_t { return added_time_; }

auto EditHistory::GetLastModifiedTime() const -> std::time_t { return last_modified_time_; }

auto EditHistory::GetHistoryId() const -> history_id_t { return history_id_; }

auto EditHistory::GetBoundImage() const -> sl_element_id_t { return bound_image_; }

auto EditHistory::GetVersion(history_id_t ver_id) -> Version& {
  EnsureDefaultVersion();
  if (version_storage_.find(ver_id) == version_storage_.end()) {
    throw std::runtime_error("Version not found");
  }
  return version_storage_[ver_id];
}

auto EditHistory::GetDefaultVersion() -> Version& {
  EnsureDefaultVersion();
  return GetVersion(default_version_id_);
}

auto EditHistory::GetActiveVersion() -> Version& {
  EnsureDefaultVersion();
  return GetVersion(active_version_id_);
}

auto EditHistory::GetActiveVersionHash() -> Hash128 {
  EnsureDefaultVersion();
  return GetVersion(active_version_id_).GetVersionHash();
}

auto EditHistory::CloneForFile(sl_element_id_t bound_image) const -> std::shared_ptr<EditHistory> {
  auto clone                     = std::make_shared<EditHistory>(bound_image);
  clone->import_pipeline_params_ = import_pipeline_params_;
  clone->active_pipeline_params_ = active_pipeline_params_;
  clone->version_order_.clear();
  clone->version_storage_.clear();

  std::unordered_map<history_id_t, history_id_t> version_id_map;
  version_id_map.reserve(version_storage_.size());

  for (const auto& [old_version_id, version] : version_storage_) {
    auto cloned_version = version.CloneForImage(bound_image);
    auto new_version_id = cloned_version.GetVersionID();
    version_id_map.emplace(old_version_id, new_version_id);
    clone->version_storage_.emplace(new_version_id, std::move(cloned_version));
  }

  if (auto default_it = version_id_map.find(default_version_id_); default_it != version_id_map.end()) {
    clone->default_version_id_ = default_it->second;
  }
  if (auto active_it = version_id_map.find(active_version_id_); active_it != version_id_map.end()) {
    clone->active_version_id_ = active_it->second;
  }

  for (const auto& node : version_order_) {
    const auto source_version_id = node.ver_ref_.GetVersionID();
    const auto mapped_it         = version_id_map.find(source_version_id);
    if (mapped_it == version_id_map.end()) {
      continue;
    }

    const auto cloned_version_it = clone->version_storage_.find(mapped_it->second);
    if (cloned_version_it == clone->version_storage_.end()) {
      continue;
    }

    clone->version_order_.emplace_back(cloned_version_it->second);
    clone->version_order_.back().commit_id_ = node.commit_id_;
  }

  clone->SetLastModifiedTime();
  clone->CalculateHistoryID();
  return clone;
}

void EditHistory::SetImportPipelineParams(nlohmann::json params) {
  import_pipeline_params_ = std::move(params);
  if (version_storage_.find(default_version_id_) != version_storage_.end()) {
    auto& default_version = version_storage_.at(default_version_id_);
    if (default_version.GetAllEditTransactions().empty()) {
      default_version.SetFinalPipelineParams(import_pipeline_params_);
      active_pipeline_params_ = import_pipeline_params_;
    }
  }
  SetLastModifiedTime();
}

auto EditHistory::ReconstructPipelineParamsForVersion(history_id_t ver_id)
    -> std::optional<nlohmann::json> {
  EnsureDefaultVersion();

  Version* version = nullptr;
  try {
    version = &GetVersion(ver_id);
  } catch (...) {
    return std::nullopt;
  }

  if (const auto snapshot = version->GetFinalPipelineParams(); snapshot.has_value()) {
    return snapshot;
  }

  try {
    CPUPipelineExecutor exec;
    exec.ImportPipelineParams(import_pipeline_params_);
    const auto& txs = version->GetAllEditTransactions();
    const size_t cursor = std::min(version->GetCursor(), txs.size());
    for (size_t i = 0; i < cursor; ++i) {
      if (!txs[i].ApplyForward(exec)) {
        return std::nullopt;
      }
    }
    return exec.ExportPipelineParams();
  } catch (...) {
    return std::nullopt;
  }
}

auto EditHistory::CreateVersion(std::string display_name) -> history_id_t {
  EnsureDefaultVersion();
  if (display_name.empty()) {
    display_name = "Version " + std::to_string(version_order_.size());
  }
  Version version = Version::Empty(bound_image_, std::move(display_name), import_pipeline_params_);
  const auto ver_id = CommitVersion(std::move(version));
  SetActiveVersionID(ver_id);
  return ver_id;
}

auto EditHistory::CommitVersion(Version&& ver) -> history_id_t {
  EnsureDefaultVersion();
  auto ver_id = ver.GetVersionID();
  if (version_storage_.find(ver_id) != version_storage_.end()) {
    throw std::runtime_error("Version already exists");
  }
  version_storage_[ver_id] = std::move(ver);
  version_order_.emplace_back(version_storage_[ver_id]);
  version_order_.back().commit_id_ = static_cast<p_hash_t>(version_order_.size());
  active_version_id_       = ver_id;
  active_pipeline_params_  = version_storage_[ver_id].GetMaterializedParams();
  SetLastModifiedTime();
  CalculateHistoryID();
  return ver_id;
}

void EditHistory::RenameVersion(history_id_t ver_id, std::string display_name) {
  EnsureDefaultVersion();
  GetVersion(ver_id).SetDisplayName(std::move(display_name));
  SetLastModifiedTime();
  CalculateHistoryID();
}

auto EditHistory::RemoveVersion(history_id_t ver_id) -> bool {
  EnsureDefaultVersion();
  if (ver_id == default_version_id_) {
    return false;
  }
  if (version_storage_.find(ver_id) == version_storage_.end()) {
    return false;
  }
  for (auto it = version_order_.begin(); it != version_order_.end(); ++it) {
    if (it->ver_ref_.GetVersionID() == ver_id) {
      version_order_.erase(it);
      break;
    }
  }
  version_storage_.erase(ver_id);
  if (active_version_id_ == ver_id) {
    active_version_id_ = default_version_id_;
    active_pipeline_params_ = version_storage_[default_version_id_].GetMaterializedParams();
  }
  SetLastModifiedTime();
  return true;
}

void EditHistory::SetActiveVersionID(history_id_t ver_id) {
  EnsureDefaultVersion();
  if (version_storage_.find(ver_id) == version_storage_.end()) {
    throw std::runtime_error("Version not found");
  }
  active_version_id_      = ver_id;
  active_pipeline_params_ = version_storage_[ver_id].GetMaterializedParams();
  SetLastModifiedTime();
}

void EditHistory::UpdateVersionFromWorkingVersion(history_id_t          ver_id,
                                                  const WorkingVersion& working_version,
                                                  const nlohmann::json& head_pipeline_params) {
  EnsureDefaultVersion();
  auto it = version_storage_.find(ver_id);
  if (it == version_storage_.end()) {
    throw std::runtime_error("Version not found");
  }
  it->second.UpdateFromWorkingVersion(working_version, head_pipeline_params);
  if (active_version_id_ == ver_id) {
    active_pipeline_params_ = head_pipeline_params;
  }
  SetLastModifiedTime();
}

auto EditHistory::ToJSON() const -> nlohmann::json {
  nlohmann::json j;
  j["history_id"]         = history_id_.ToString();
  j["bound_image"]        = bound_image_;
  j["added_time"]         = added_time_;
  j["last_modified_time"] = last_modified_time_;
  j["default_version_id"] = default_version_id_.ToString();
  j["active_version_id"]  = active_version_id_.ToString();
  j["import_pipeline_params"] = import_pipeline_params_;
  if (active_pipeline_params_.has_value()) {
    j["active_pipeline_params"] = *active_pipeline_params_;
  }

  j["version_order"] = nlohmann::json::array();
  for (const auto& node : version_order_) {
    nlohmann::json node_json;
    node_json["order"]       = node.commit_id_;
    node_json["version_id"] = node.ver_ref_.GetVersionID().ToString();
    j["version_order"].push_back(node_json);
  }

  auto append_version = [&j](const history_id_t& ver_id, const Version& ver) {
    nlohmann::json ver_json;
    ver_json["version_id"] = ver_id.ToString();
    ver_json["version"]    = ver.ToJSON();
    j["version_storage"].push_back(ver_json);
  };

  j["version_storage"] = nlohmann::json::array();
  std::unordered_map<std::string, bool> emitted;
  for (const auto& node : version_order_) {
    const auto ver_id = node.ver_ref_.GetVersionID();
    append_version(ver_id, node.ver_ref_);
    emitted[ver_id.ToString()] = true;
  }
  for (const auto& [ver_id, ver] : version_storage_) {
    if (emitted.find(ver_id.ToString()) != emitted.end()) {
      continue;
    }
    append_version(ver_id, ver);
  }

  return j;
}

void EditHistory::FromJSON(const nlohmann::json& j) {
  if (!j.is_object() || !j.contains("history_id") || !j.contains("bound_image") ||
      !j.contains("added_time") || !j.contains("last_modified_time") ||
      !j.contains("default_version_id") || !j.contains("active_version_id") ||
      !j.contains("import_pipeline_params") || !j.contains("version_order") ||
      !j.contains("version_storage")) {
    throw std::runtime_error("EditHistory: Invalid JSON format for EditHistory");
  }

  history_id_           = Hash128::FromString(j.at("history_id").get<std::string>());
  bound_image_          = j.at("bound_image").get<sl_element_id_t>();
  added_time_           = j.at("added_time").get<std::time_t>();
  last_modified_time_   = j.at("last_modified_time").get<std::time_t>();
  default_version_id_   = Hash128::FromString(j.at("default_version_id").get<std::string>());
  active_version_id_    = Hash128::FromString(j.at("active_version_id").get<std::string>());
  import_pipeline_params_ = j.at("import_pipeline_params");
  active_pipeline_params_ = j.contains("active_pipeline_params")
                                ? std::optional<nlohmann::json>(j.at("active_pipeline_params"))
                                : std::nullopt;

  version_order_.clear();
  version_storage_.clear();
  for (const auto& ver_json : j.at("version_storage")) {
    if (!ver_json.is_object() || !ver_json.contains("version")) {
      version_storage_.clear();
      throw std::runtime_error("EditHistory: Invalid JSON format for version_storage node");
    }
    Version ver;
    ver.FromJSON(ver_json.at("version"));
    history_id_t ver_id      = ver.GetVersionID();
    version_storage_[ver_id] = std::move(ver);
  }

  for (const auto& node_json : j.at("version_order")) {
    if (!node_json.is_object() || !node_json.contains("order") ||
        !node_json.contains("version_id")) {
      version_order_.clear();
      version_storage_.clear();
      throw std::runtime_error("EditHistory: Invalid JSON format for version_order node");
    }
    const history_id_t ver_id =
        Hash128::FromString(node_json.at("version_id").get<std::string>());
    auto it = version_storage_.find(ver_id);
    if (it == version_storage_.end()) {
      continue;
    }
    VersionNode node(it->second);
    node.commit_id_ = node_json.at("order").get<p_hash_t>();
    version_order_.push_back(std::move(node));
  }

  EnsureDefaultVersion();
}
};  // namespace alcedo
