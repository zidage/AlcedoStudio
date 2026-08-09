//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/sleeve_filter_service.hpp"
#include "app/sleeve_service.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "type/type.hpp"

namespace alcedo {

struct AlbumFolderView {
  sl_element_id_t       folder_id_ = 0;
  file_name_t           folder_name_{};
  std::filesystem::path folder_path_{};
};

enum class AlbumScopeType {
  Root,
  Album,
};

struct AlbumFileView {
  sl_element_id_t       element_id_ = 0;
  sl_element_id_t       file_id_    = 0;
  image_id_t            image_id_   = 0;
  sl_element_id_t       folder_id_  = 0;
  AlbumScopeType        scope_type_ = AlbumScopeType::Root;
  file_name_t           file_name_{};
  std::filesystem::path file_path_{};
};

struct AlbumDeleteResult {
  std::vector<AlbumFileView>         deleted_files_{};
  std::vector<std::filesystem::path> failed_paths_{};
  std::vector<sl_element_id_t>       failed_element_ids_{};
};

class AlbumBrowseService {
 public:
  explicit AlbumBrowseService(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
                              std::shared_ptr<SleeveFilterService> filter_service = nullptr)
      : sleeve_service_(std::move(sleeve_service)), filter_service_(std::move(filter_service)) {}

  [[nodiscard]] auto ListFolders(const std::filesystem::path& folder_path) const
      -> std::vector<AlbumFolderView>;
  [[nodiscard]] auto ListFilesInFolder(const std::filesystem::path& folder_path) const
      -> std::vector<AlbumFileView>;
  [[nodiscard]] auto ListFilesInFolderById(sl_element_id_t folder_id) const
      -> std::vector<AlbumFileView>;
  [[nodiscard]] auto ListFilesInFolderById(
      sl_element_id_t                            folder_id, size_t offset, size_t limit,
      const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt) const
      -> std::vector<AlbumFileView>;
  [[nodiscard]] auto CountFilesInFolderById(
      sl_element_id_t                            folder_id,
      const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt) const -> size_t;

  [[nodiscard]] auto CreateFolder(const std::filesystem::path& parent_folder_path,
                                  const file_name_t& name) -> std::optional<AlbumFolderView>;
  [[nodiscard]] bool DeleteFolder(const std::filesystem::path& folder_path);
  [[nodiscard]] auto DeleteFiles(const std::vector<std::filesystem::path>& file_paths)
      -> AlbumDeleteResult;
  [[nodiscard]] auto DeleteFilesByElementIds(const std::vector<sl_element_id_t>& element_ids)
      -> AlbumDeleteResult;
  [[nodiscard]] auto DeleteFilesInFolderByElementIds(
      sl_element_id_t folder_id, const std::vector<sl_element_id_t>& element_ids)
      -> AlbumDeleteResult;
  [[nodiscard]] auto LinkFilesToFolder(const std::vector<sl_element_id_t>& element_ids,
                                       sl_element_id_t target_folder_id) -> AlbumDeleteResult;

 private:
  std::shared_ptr<SleeveServiceImpl>   sleeve_service_{};
  std::shared_ptr<SleeveFilterService> filter_service_{};
};

}  // namespace alcedo
