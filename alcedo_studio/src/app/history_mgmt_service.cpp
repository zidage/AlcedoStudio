//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/history_mgmt_service.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "type/type.hpp"

namespace alcedo {
void EditHistoryMgmtService::HandleEviction(sl_element_id_t evicted_id) {
  // If the would-be evicted history is pinned, keep it and evict another entry instead.
  // This service is typically single-session/single-history, so pinned guards are expected.
  sl_element_id_t candidate    = evicted_id;
  const size_t    max_attempts = cached_histories_.empty() ? 1 : (cached_histories_.size() + 1);

  for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
    auto it = cached_histories_.find(candidate);
    if (it == cached_histories_.end()) {
      return;
    }

    auto history_guard = it->second;
    if (!history_guard->pinned_) {
      if (history_guard->dirty_) {
        storage_->GetElementStore().UpdateEditHistoryByFileId(candidate,
                                                                          history_guard->history_);
        history_guard->dirty_ = false;
      }
      cached_histories_.erase(it);
      cache_.RemoveRecord(candidate);
      return;
    }

    // Pinned: put it back into the LRU and evict a different entry.
    auto next = cache_.RecordAccess_WithEvict(candidate, candidate);
    if (!next.has_value()) {
      return;
    }
    candidate = next.value();
  }

  // Fallback: if everything is pinned, allow temporary growth to avoid evicting in-use histories.
  auto keys = cache_.GetLRUKeys();
  cache_.Resize(static_cast<uint32_t>(keys.size() + 5));
  cache_.RecordAccess(evicted_id, evicted_id);
}

auto EditHistoryMgmtService::LoadHistory(sl_element_id_t file_id)
    -> std::shared_ptr<EditHistoryGuard> {
  std::unique_lock<std::mutex> guard(lock_);

  if (cache_.Contains(file_id)) {
    auto cached_id = cache_.AccessElement(file_id);
    if (cached_id.has_value()) {
      auto it = cached_histories_.find(cached_id.value());
      if (it != cached_histories_.end()) {
        // Always pin on load; this service is intended for a single active history per session.
        it->second->pinned_  = true;
        it->second->file_id_ = file_id;
        return it->second;
      }
    }
  }

  std::shared_ptr<EditHistory>      history;
  std::shared_ptr<EditHistoryGuard> history_guard;
  history = storage_->GetLiveEditHistory(file_id);
  try {
    if (!history) {
      history = storage_->GetElementStore().GetEditHistoryByFileId(file_id);
    }
  } catch (std::exception& e) {
    throw std::runtime_error(
        "[ERROR] EditHistoryMgmtService: Failed to load edit history from storage for file ID " +
        std::to_string(file_id) + ": " + e.what());
  }

  if (!history) {
    history = std::make_shared<EditHistory>(file_id);
  }

  history_guard            = std::make_shared<EditHistoryGuard>();
  history_guard->file_id_  = file_id;
  history_guard->history_  = std::move(history);
  history_guard->dirty_    = false;
  history_guard->pinned_   = true;
  storage_->RememberLiveEditHistory(file_id, history_guard->history_);

  std::optional<sl_element_id_t> evicted = cache_.RecordAccess_WithEvict(file_id, file_id);
  if (evicted.has_value()) {
    HandleEviction(evicted.value());
  }

  cached_histories_[file_id] = history_guard;

  // If no eviction happened, and the cache size is still in "boost" range, resize it
  if (!evicted.has_value() && cached_histories_.size() + 1 > default_cache_capacity_) {
    cache_.Resize(static_cast<uint32_t>(cached_histories_.size() - 1));
  }

  return history_guard;
}

auto EditHistoryMgmtService::CommitVersion(const std::shared_ptr<EditHistoryGuard>& history_guard,
                                          Version&&                                version)
    -> history_id_t {
  if (!history_guard || !history_guard->history_) {
    throw std::runtime_error("[ERROR] EditHistoryMgmtService: CommitVersion called with null guard");
  }

  std::unique_lock<std::mutex> guard(lock_);
  const sl_element_id_t file_id = history_guard->file_id_;
  std::optional<sl_element_id_t> evicted = cache_.RecordAccess_WithEvict(file_id, file_id);
  if (evicted.has_value()) {
    HandleEviction(evicted.value());
  }
  cached_histories_[file_id] = history_guard;
  history_guard->pinned_     = true;

  const history_id_t committed_id = history_guard->history_->CommitVersion(std::move(version));
  history_guard->dirty_           = true;
  return committed_id;
}

auto EditHistoryMgmtService::CreateVersion(const std::shared_ptr<EditHistoryGuard>& history_guard,
                                          std::string display_name) -> history_id_t {
  if (!history_guard || !history_guard->history_) {
    throw std::runtime_error("[ERROR] EditHistoryMgmtService: CreateVersion called with null guard");
  }

  std::unique_lock<std::mutex> guard(lock_);
  const sl_element_id_t file_id = history_guard->file_id_;
  std::optional<sl_element_id_t> evicted = cache_.RecordAccess_WithEvict(file_id, file_id);
  if (evicted.has_value()) {
    HandleEviction(evicted.value());
  }
  cached_histories_[file_id] = history_guard;
  history_guard->pinned_     = true;

  const auto version_id = history_guard->history_->CreateVersion(std::move(display_name));
  history_guard->dirty_ = true;
  return version_id;
}

void EditHistoryMgmtService::RenameVersion(const std::shared_ptr<EditHistoryGuard>& history_guard,
                                           history_id_t version_id, std::string display_name) {
  if (!history_guard || !history_guard->history_) {
    throw std::runtime_error("[ERROR] EditHistoryMgmtService: RenameVersion called with null guard");
  }
  std::unique_lock<std::mutex> guard(lock_);
  history_guard->history_->RenameVersion(version_id, std::move(display_name));
  history_guard->dirty_ = true;
}

void EditHistoryMgmtService::SetActiveVersion(
    const std::shared_ptr<EditHistoryGuard>& history_guard, history_id_t version_id) {
  if (!history_guard || !history_guard->history_) {
    throw std::runtime_error("[ERROR] EditHistoryMgmtService: SetActiveVersion called with null guard");
  }
  std::unique_lock<std::mutex> guard(lock_);
  history_guard->history_->SetActiveVersionID(version_id);
  history_guard->dirty_ = true;
}

void EditHistoryMgmtService::UpdateVersion(
    const std::shared_ptr<EditHistoryGuard>& history_guard, history_id_t version_id,
    const WorkingVersion& working_version, const nlohmann::json& head_pipeline_params) {
  if (!history_guard || !history_guard->history_) {
    throw std::runtime_error("[ERROR] EditHistoryMgmtService: UpdateVersion called with null guard");
  }
  std::unique_lock<std::mutex> guard(lock_);
  history_guard->history_->UpdateVersionFromWorkingVersion(version_id, working_version,
                                                           head_pipeline_params);
  history_guard->dirty_ = true;
}

void EditHistoryMgmtService::SaveHistory(const std::shared_ptr<EditHistoryGuard>& history_guard) {
  if (!history_guard) {
    return;
  }

  std::unique_lock<std::mutex> guard(lock_);

  const sl_element_id_t file_id = history_guard->file_id_;

  // Ensure the guard remains tracked by the cache even if constructed externally.
  std::optional<sl_element_id_t> evicted = cache_.RecordAccess_WithEvict(file_id, file_id);
  if (evicted.has_value()) {
    HandleEviction(evicted.value());
  }

  cached_histories_[file_id] = history_guard;
  storage_->RememberLiveEditHistory(file_id, history_guard->history_);

  if (history_guard->dirty_) {
    storage_->GetElementStore().UpdateEditHistoryByFileId(file_id,
                                                                      history_guard->history_);
    history_guard->dirty_ = false;
  }

  // Return to cache (unpinned)
  history_guard->pinned_ = false;

  // If eviction did not happen, but the cache size is still in "boost" range, resize it
  if (!evicted.has_value() && cached_histories_.size() + 1 > default_cache_capacity_) {
    cache_.Resize(static_cast<uint32_t>(cached_histories_.size() - 1));
  }
}

void EditHistoryMgmtService::DeleteHistory(sl_element_id_t file_id) {
  std::unique_lock<std::mutex> guard(lock_);
  cache_.RemoveRecord(file_id);
  cached_histories_.erase(file_id);
  storage_->ForgetLiveEditHistory(file_id);
  try {
    storage_->GetElementStore().RemoveEditHistoryByFileId(file_id);
  } catch (...) {
  }
}

void EditHistoryMgmtService::DeleteHistories(std::span<const sl_element_id_t> file_ids) {
  std::unique_lock<std::mutex> guard(lock_);
  for (const auto file_id : file_ids) {
    if (file_id == 0) {
      continue;
    }
    cache_.RemoveRecord(file_id);
    cached_histories_.erase(file_id);
    storage_->ForgetLiveEditHistory(file_id);
  }
  try {
    storage_->GetElementStore().RemoveEditHistoriesByFileIds(file_ids);
  } catch (...) {
  }
}

void EditHistoryMgmtService::Sync() {
  std::unique_lock<std::mutex> guard(lock_);
  for (auto& [file_id, history_guard] : cached_histories_) {
    if (!history_guard || !history_guard->dirty_) {
      continue;
    }
    storage_->GetElementStore().UpdateEditHistoryByFileId(file_id,
                                                                      history_guard->history_);
    history_guard->dirty_ = false;
  }
}
}  // namespace alcedo
