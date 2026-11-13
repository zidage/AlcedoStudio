//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include "edit/history/edit_history.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"

namespace alcedo {
struct EditHistoryGuard {
  sl_element_id_t              file_id_;
  std::shared_ptr<EditHistory> history_;

  bool                         dirty_  = false;
  bool                         pinned_ = false;
};

class EditHistoryMgmtService final {
 private:
  std::shared_ptr<Storage>            storage_;

  LRUCache<sl_element_id_t, sl_element_id_t> cache_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<EditHistoryGuard>> cached_histories_;

  std::mutex                                  lock_;

  static constexpr size_t                     default_cache_capacity_ = 16;

  void HandleEviction(sl_element_id_t evicted_id);

 public:
  EditHistoryMgmtService() = delete;
  explicit EditHistoryMgmtService(std::shared_ptr<Storage> storage_service)
      : storage_(std::move(storage_service)),
        cache_(default_cache_capacity_),
        cached_histories_() {}

  auto LoadHistory(sl_element_id_t file_id) -> std::shared_ptr<EditHistoryGuard>;

  auto CommitVersion(const std::shared_ptr<EditHistoryGuard>& history_guard, Version&& version)
      -> history_id_t;
  auto CreateVersion(const std::shared_ptr<EditHistoryGuard>& history_guard,
                     std::string display_name = {}) -> history_id_t;
  void RenameVersion(const std::shared_ptr<EditHistoryGuard>& history_guard, history_id_t version_id,
                     std::string display_name);
  void SetActiveVersion(const std::shared_ptr<EditHistoryGuard>& history_guard,
                        history_id_t version_id);
  void UpdateVersion(const std::shared_ptr<EditHistoryGuard>& history_guard, history_id_t version_id,
                     const WorkingVersion& working_version,
                     const nlohmann::json& head_pipeline_params);

  void SaveHistory(const std::shared_ptr<EditHistoryGuard>& history_guard);
  void DeleteHistory(sl_element_id_t file_id);
  void DeleteHistories(std::span<const sl_element_id_t> file_ids);

  void Sync();
};
};  // namespace alcedo
