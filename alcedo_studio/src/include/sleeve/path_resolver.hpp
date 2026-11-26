//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dentry_cache_manager.hpp"
#include "sleeve/node_storage_handler.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
class PathResolver {
 private:
  std::shared_ptr<SleeveFolder>        root_;
  LRUCache<sl_path_t, sl_element_id_t> directory_cache_;
  NodeStorageHandler&                  storage_handler_;
  IncrID::IDGenerator<uint32_t>&       id_gen_;

  auto                                 CoWHandler(const std::shared_ptr<SleeveElement>& to_copy,
                                                  const std::shared_ptr<SleeveFolder>&  parent_folder)
      -> std::shared_ptr<SleeveElement>;

 public:
  PathResolver();
  PathResolver(NodeStorageHandler& lazy_handler, IncrID::IDGenerator<uint32_t>& id_gen);
  void        SetRoot(std::shared_ptr<SleeveFolder> root);
  static auto Normalize(const std::filesystem::path raw_path) -> std::wstring;

  auto IsSubpath(const std::filesystem::path& base, const std::filesystem::path& target) -> bool;
  auto Contains(const std::filesystem::path& path, ElementType type) -> bool;
  auto Contains(const std::filesystem::path& path) -> bool;
  /**
   * @brief The live element at @p path. Each part reads the resident child folders of its
   *        folder. Only a part that names no child folder loads the file entries of its folder
   *        (NodeStorageHandler::EnsureChildrenLoaded), so a folder path reads memory only. The
   *        file entries of the result are not loaded.
   * @throws std::runtime_error when a part names no live element or a part below a file.
   */
  auto Resolve(const std::filesystem::path& path) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The live folder at @p path, from the resident child folders of each folder on the
   *        path. Reads memory only and loads no file entry.
   * @throws std::runtime_error when a part of the path names no live folder.
   */
  auto ResolveFolder(const std::filesystem::path& path) -> std::shared_ptr<SleeveFolder>;
  /**
   * @brief The element at @p path for a change: a shared folder on the path is copied (CoW),
   *        the result is marked modified, and each folder on the path and a folder result have
   *        their file entries loaded, so the name checks of a write see every entry.
   * @throws std::runtime_error when a part names no element or a part below a file.
   */
  auto ResolveForWrite(const std::filesystem::path& path) -> std::shared_ptr<SleeveElement>;
  void Invalidate(const std::filesystem::path& path);
  /// Removes every cached path. A rename or a move changes the path of a whole subtree.
  void InvalidateAll();

  auto Tree(const std::filesystem::path& path) -> std::wstring;
};
};  // namespace alcedo
