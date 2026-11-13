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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/sleeve_filter_service.hpp"
#include "app/sleeve_service.hpp"
#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/store/semantic/semantic_records.hpp"
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

/// Outcome of a folder rename or move.
enum class AlbumFolderEditStatus {
  Done,           ///< The change is applied and synced (or it changes nothing).
  NameTaken,      ///< The destination folder holds another element with the same name.
  InvalidTarget,  ///< Root folder, empty name, missing folder, or a move into its own subtree.
  Failed,         ///< The storage write or the sync failed.
};

struct AlbumFolderEditResult {
  AlbumFolderEditStatus status_ = AlbumFolderEditStatus::Failed;
  /// Path of the folder after the change; set only when the status is Done.
  std::filesystem::path folder_path_{};
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
  /// One page of the files of @p folder_id that match @p filter (all files when empty), in
  /// element id order; @p limit 0 lists every file. Empty when a read fails.
  [[nodiscard]] auto ListFilesInFolderById(
      sl_element_id_t folder_id, size_t offset, size_t limit,
      const std::optional<FilterNode>& filter = std::nullopt) const -> std::vector<AlbumFileView>;
  /// Number of the files of @p folder_id that match @p filter; 0 when a read fails.
  [[nodiscard]] auto CountFilesInFolderById(
      sl_element_id_t folder_id, const std::optional<FilterNode>& filter = std::nullopt) const
      -> size_t;
  /// Photo count, edited photo count, and capture date range of the whole project. Reads only.
  /// Unlike the listing reads, it does not hide a failure: it throws std::runtime_error when the
  /// sleeve service is absent or the statement fails.
  [[nodiscard]] auto ReadProjectOverview() const -> ProjectOverviewCounts;
  /// Photo count of the whole project and of every folder from one grouped statement (see
  /// ElementStore::ReadFolderImageCounts). Throws like ReadAlbumQuery.
  /// Thread: may run on a worker thread.
  [[nodiscard]] auto ReadFolderImageCounts() const -> FolderImageCounts;

  /**
   * @brief The one library read: Inspector filters and search, sort, groups, statistics, and
   *        one bounded page of the ordered photo occurrences of @p folder_id.
   *
   * @param filter Merged search and Inspector filter tree; compiled once here.
   * @param active_semantic_model_key Key the caller read once from its owner; the label
   *        filter, groups, sort, and statistics all use it.
   * @param semantic_relevance The embeddings of a natural-language search
   *        (SleeveFilterService::EmbedSemanticQuery). When set, only the files of @p folder_id
   *        that the relevance rule keeps match, and without a selected sort they order by
   *        relevance (see ElementStore::ReadAlbumQuery).
   * @throws std::invalid_argument for invalid options or page bounds.
   * @throws std::runtime_error when the project storage is missing or a statement fails. A
   *         failure never yields an empty successful result.
   *
   * Thread: may run on a worker thread; reads storage under its connection lock.
   */
  [[nodiscard]] auto ReadAlbumQuery(
      sl_element_id_t folder_id, const std::optional<FilterNode>& filter,
      const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
      const AlbumQueryRead&                        read,
      const std::optional<SemanticQueryEmbedding>& semantic_relevance = std::nullopt) const
      -> AlbumQueryResult;
  /// Position of @p file_id in the ordered occurrences of the same query (see ReadAlbumQuery).
  [[nodiscard]] auto ReadAlbumFilePosition(
      sl_element_id_t folder_id, const std::optional<FilterNode>& filter,
      const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
      sl_element_id_t file_id, const std::optional<AlbumGroupKey>& preferred_group,
      const std::optional<SemanticQueryEmbedding>& semantic_relevance = std::nullopt) const
      -> std::optional<AlbumFilePosition>;
  /// Ordered unique file ids of the occurrences in [@p begin, @p end) of the same query.
  [[nodiscard]] auto ReadAlbumFileIds(
      sl_element_id_t folder_id, const std::optional<FilterNode>& filter,
      const AlbumQueryOptions& options, const std::string& active_semantic_model_key, int64_t begin,
      int64_t                                      end,
      const std::optional<SemanticQueryEmbedding>& semantic_relevance = std::nullopt) const
      -> std::vector<sl_element_id_t>;

  /// Display rows (file and image ids, name, typed metadata) of @p file_ids in that order.
  /// Reads typed columns only, no thumbnail and no image pool data. Throws like ReadAlbumQuery.
  [[nodiscard]] auto ReadAlbumFileRows(std::span<const sl_element_id_t> file_ids) const
      -> std::vector<SearchResultRow>;

  /// The project file whose active Version head commit is the newest (see
  /// ElementStore::ReadLastEditedFile); empty when no file is edited. Throws like
  /// ReadAlbumQuery. Thread: may run on a worker thread.
  [[nodiscard]] auto ReadLastEditedFile() const -> std::optional<LastEditedFile>;

  /// Receives the operation name at the start of each album query read, on the thread that
  /// runs it. Tests install one to prove that album SQL leaves the UI thread. Thread-safe.
  using QueryThreadObserver = std::function<void(std::string_view operation)>;
  void               SetQueryThreadObserver(QueryThreadObserver observer);

  [[nodiscard]] auto CreateFolder(const std::filesystem::path& parent_folder_path,
                                  const file_name_t& name) -> std::optional<AlbumFolderView>;
  [[nodiscard]] bool DeleteFolder(const std::filesystem::path& folder_path);
  /// Renames the folder at @p folder_path; it keeps its id, children, and album membership.
  [[nodiscard]] auto RenameFolder(const std::filesystem::path& folder_path,
                                  const file_name_t& new_name) -> AlbumFolderEditResult;
  /// Moves the folder at @p folder_path into @p new_parent_path (the root path moves it to the
  /// top level); it keeps its id, name, children, and album membership.
  [[nodiscard]] auto MoveFolder(const std::filesystem::path& folder_path,
                                const std::filesystem::path& new_parent_path)
      -> AlbumFolderEditResult;
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
