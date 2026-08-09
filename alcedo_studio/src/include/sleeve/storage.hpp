//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/database.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/semantic/semantic_store.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "type/type.hpp"

namespace alcedo {
class CPUPipelineExecutor;
class EditHistory;

class NodeStorageHandler {
 private:
  ElementStore&                                                   element_store_;

  std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>& storage_;

 public:
  NodeStorageHandler(ElementStore&                                                   element_store,
                     std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>& storage);
  void AddToStorage(std::shared_ptr<SleeveElement> new_element);
  void EnsureChildrenLoaded(std::shared_ptr<SleeveFolder> folder);
  auto GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  void GarbageCollect();
};

class Storage {
 private:
  Database                                                              database_;
  ElementStore                                                         element_store_;
  ImageStore                                                           image_store_;
  SemanticStore                                                 semantic_store_;
  AiStore                                                       ai_store_;
  std::mutex                                                                live_state_lock_;

  std::unordered_map<sl_element_id_t, std::weak_ptr<EditHistory>>           live_histories_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<CPUPipelineExecutor>> live_pipelines_;

 public:
  Storage(std::filesystem::path db_path);

  auto GetDatabase() -> Database&;
  auto GetElementStore() -> ElementStore&;
  auto GetImageStore() -> ImageStore&;
  auto GetSemanticStore() -> SemanticStore&;
  auto GetAiStore() -> AiStore&;

  void RememberLiveEditHistory(sl_element_id_t                     file_id,
                               const std::shared_ptr<EditHistory>& history);
  auto GetLiveEditHistory(sl_element_id_t file_id) -> std::shared_ptr<EditHistory>;
  void ForgetLiveEditHistory(sl_element_id_t file_id);

  void RememberLivePipeline(sl_element_id_t                             file_id,
                            const std::shared_ptr<CPUPipelineExecutor>& pipeline);
  auto GetLivePipeline(sl_element_id_t file_id) -> std::shared_ptr<CPUPipelineExecutor>;
  void ForgetLivePipeline(sl_element_id_t file_id);
};
};  // namespace alcedo
