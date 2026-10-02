//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/sleeve_filter_service.hpp"
#include "app/sleeve_service.hpp"
#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/store/sleeve/element_store.hpp"
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
  /// Photo count, edited photo count, and capture date range of the whole project. Reads only.
  /// Unlike the listing reads, it does not hide a failure: it throws std::runtime_error when the
  /// sleeve service is absent or the statement fails.
  [[nodiscard]] auto ReadProjectOverview() const -> ProjectOverviewCounts;

  /**
   * @brief The one library read: Inspector filters and search, sort, groups, statistics, and
   *        one bounded page of the ordered photo occurrences of @p folder_id.
   *
   * @param filter Merged search and Inspector filter tree; compiled once here.
   * @param active_semantic_model_key Key the caller read once from its owner; the label
   *        filter, groups, sort, and statistics all use it.
   * @throws std::invalid_argument for invalid options or page bounds.
   * @throws std::runtime_error when the project storage is missing or a statement fails. A
   *         failure never yields an empty successful result.
   *
   * Thread: may run on a worker thread; reads storage under its connection lock.
   */
  [[nodiscard]] auto ReadAlbumQuery(sl_element_id_t                  folder_id,
                                    const std::optional<FilterNode>& filter,
                                    const AlbumQueryOptions&         options,
                                    const std::string&               active_semantic_model_key,
                                    const AlbumQueryRead& read) const -> AlbumQueryResult;
  /// Position of @p file_id in the ordered occurrences of the same query (see ReadAlbumQuery).
  [[nodiscard]] auto ReadAlbumFilePosition(
      sl_element_id_t folder_id, const std::optional<FilterNode>& filter,
      const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
      sl_element_id_t file_id, const std::optional<AlbumGroupKey>& preferred_group) const
      -> std::optional<AlbumFilePosition>;
  /// Ordered unique file ids of the occurrences in [@p begin, @p end) of the same query.
  [[nodiscard]] auto ReadAlbumFileIds(sl_element_id_t                  folder_id,
                                      const std::optional<FilterNode>& filter,
                                      const AlbumQueryOptions&         options,
                                      const std::string& active_semantic_model_key, int64_t begin,
                                      int64_t end) const -> std::vector<sl_element_id_t>;

  /// Receives the operation name at the start of each album query read, on the thread that
  /// runs it. Tests install one to prove that album SQL leaves the UI thread. Thread-safe.
  using QueryThreadObserver = std::function<void(std::string_view operation)>;
  void               SetQueryThreadObserver(QueryThreadObserver observer);

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
  [[nodiscard]] auto ElementStoreForRead(std::string_view operation) const -> ElementStore&;

  std::shared_ptr<SleeveServiceImpl>   sleeve_service_{};
  std::shared_ptr<SleeveFilterService> filter_service_{};
  mutable std::mutex                   query_thread_observer_mutex_;
  QueryThreadObserver                  query_thread_observer_{};
};

}  // namespace alcedo
