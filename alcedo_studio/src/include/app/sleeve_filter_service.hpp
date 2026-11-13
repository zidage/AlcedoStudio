//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "image/color_label.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/library_search_request.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {

struct StatsBucket {
  std::string label_{};
  int         count_ = 0;
};

struct AlbumStatsView {
  int                      total_photo_count_ = 0;
  std::vector<StatsBucket> date_stats_{};
  std::vector<StatsBucket> camera_stats_{};
  std::vector<StatsBucket> lens_stats_{};
  std::vector<StatsBucket> label_stats_{};
  std::vector<StatsBucket> rating_stats_{};
  /// Local import-day buckets; read only when the library query names a time zone.
  std::vector<StatsBucket> import_date_stats_{};
  /// Photos of each color label code (index = ColorLabel), and photos with the Delete mark.
  std::array<int, kColorLabelCount> color_label_counts_{};
  int                               delete_mark_count_ = 0;
};

/// The statistics of one storage read in the application form.
[[nodiscard]] auto ToAlbumStatsView(const FolderStatsView& stats) -> AlbumStatsView;

/// One page of the files that match a filter and the stats of all matching files.
struct SearchResultPageAndStats {
  SearchResultPage page_{};
  AlbumStatsView   stats_{};
};

struct FuzzySearchMatch {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};
};

/// Embeds natural-language query text for the active semantic model of the project.
class SemanticSearchProvider {
 public:
  virtual ~SemanticSearchProvider() = default;

  /**
   * @brief The embeddings of @p query and of SemanticVectorSearch::kNeutralText.
   *
   * @p query is the trimmed query text. Both vectors are validated against the active model
   * dimension. May start the semantic sidecar and block until it embeds the text.
   * @throws std::runtime_error when no model is active, the sidecar fails to start or to
   *         embed, or a returned vector is invalid.
   */
  [[nodiscard]] virtual auto EmbedQuery(const std::wstring& query) -> SemanticQueryEmbedding = 0;

  /**
   * @brief Embeds SemanticVectorSearch::kNeutralText for the active model into the query
   *        embedding cache, so that the first search after it needs no model load.
   *
   * May start the semantic sidecar and block until it embeds the text. The default does
   * nothing. @throws std::runtime_error on the failures of EmbedQuery.
   */
  virtual void               Prewarm() {}

  /// True when the neutral text of the active model is cached (a Prewarm has nothing to do).
  /// Reads the active model from storage. The default returns true.
  [[nodiscard]] virtual auto IsPrewarmed() -> bool { return true; }
};

/// Threading: the const query methods (search, count, stats, WHERE build) read storage under
/// its connection lock and may run on a worker thread while the UI thread uses the service.
/// The filter combo storage and result caches are not synchronized: call CreateFilterCombo,
/// ApplyFilterOn, InvalidateResultCache, and SetSemanticSearchProvider from one thread only.
class SleeveFilterService {
 public:
  /// Receives the operation name at the start of each search or stats query, on the thread
  /// that runs the query. Tests install one to prove that search SQL leaves the UI thread.
  using QueryThreadObserver = std::function<void(std::string_view operation)>;

 private:
  std::shared_ptr<Storage>                       storage_;
  std::shared_ptr<SemanticSearchProvider>               semantic_search_provider_{};
  mutable std::mutex                                    query_thread_observer_mutex_;
  QueryThreadObserver                                   query_thread_observer_{};

  // Filter will not be saved in DB for now. It will be only stored in memory for the lifetime of
  // the application.
  IncrID::IDGenerator<filter_id_t>                      filter_id_generator_;

  LRUCache<filter_id_t, std::shared_ptr<FilterCombo>>   filter_storage_;
  LRUCache<std::uint64_t, std::vector<sl_element_id_t>> filter_result_cache_;

 public:
  // Disable all copy operations
  SleeveFilterService()                                      = delete;
  SleeveFilterService(const SleeveFilterService&)            = delete;
  SleeveFilterService& operator=(const SleeveFilterService&) = delete;

  SleeveFilterService(std::shared_ptr<Storage> storage_service)
      : storage_(std::move(storage_service)), filter_id_generator_(0) {}

  auto CreateFilterCombo(const FilterNode& root) -> filter_id_t;
  auto GetFilterCombo(filter_id_t filter_id) -> std::optional<std::shared_ptr<FilterCombo>>;
  void RemoveFilterCombo(filter_id_t filter_id);
  auto ApplyFilterOn(filter_id_t filter_id, sl_element_id_t parent_id)
      -> std::optional<std::vector<sl_element_id_t>>;
  /// Stats buckets of the files that match @p extra_filter (all files when empty). The filter
  /// is evaluated once for all buckets. Throws std::runtime_error when a query fails.
  auto BuildFolderStats(sl_element_id_t                  parent_id,
                        const std::optional<FilterNode>& extra_filter = std::nullopt) const
      -> AlbumStatsView;
  /// Build a filter tree for a fuzzy-search query. SearchQueryParser splits the
  /// query into typed terms (date, file kind, capture parameter, text); every
  /// term must match. A typed term matches its column or its folded text in
  /// the enabled search text columns; a text term matches only the text
  /// columns (and the CLIP labels under AiTags). The returned node is a
  /// LibrarySearch node: the parsed request and the capabilities (active
  /// semantic model, AI full-text index) read once here; storage builds the
  /// predicate (BuildLibrarySearchPredicate). Returns std::nullopt for an
  /// empty query; with @p mask 0 the node matches no file (distinct from
  /// "no filter").
  [[nodiscard]] auto BuildFuzzySearchWhere(const std::wstring& query,
                                           SearchFieldMask      mask = kAllSearchFields) const
      -> std::optional<FilterNode>;
  /// Build a filter tree that matches exactly one file (a FileId condition).
  [[nodiscard]] auto BuildExactFileWhere(sl_element_id_t file_id) const -> FilterNode;
  [[nodiscard]] auto SearchFolder(sl_element_id_t parent_id, const std::wstring& query,
                                  size_t offset = 0, size_t limit = 48,
                                  SearchFieldMask mask = kAllSearchFields) const
      -> std::vector<FuzzySearchMatch>;
  /// One page of fuzzy-search results with the display columns and the total match count,
  /// from one SQL statement. Returns an empty page for an empty query.
  [[nodiscard]] auto SearchFolderPage(sl_element_id_t parent_id, const std::wstring& query,
                                      size_t offset, size_t limit,
                                      SearchFieldMask mask = kAllSearchFields) const
      -> SearchResultPage;
  /// One page of the files that match @p filter (all files when @p filter is empty), with
  /// the display columns and the total, from one SQL statement.
  [[nodiscard]] auto ListSearchResultPage(sl_element_id_t                  parent_id,
                                          const std::optional<FilterNode>& filter, size_t offset,
                                          size_t limit) const -> SearchResultPage;
  /// One page of the files that match @p filter, the total, and the BuildFolderStats buckets,
  /// from one evaluation of the filter. This is what applying a search to the grid reads.
  /// Throws std::runtime_error when a query fails.
  [[nodiscard]] auto ListSearchResultPageWithStats(sl_element_id_t                  parent_id,
                                                   const std::optional<FilterNode>& filter,
                                                   size_t offset, size_t limit) const
      -> SearchResultPageAndStats;
  /// Semantic (CLIP) search: one page of the files kept by the relevance rule of
  /// SemanticVectorSearch, with their display columns, in relevance order, and the number of
  /// kept files. Empty when no provider is set or the query is empty.
  /// @throws std::runtime_error when the embedding or the ranking fails.
  [[nodiscard]] auto SearchFolderSemanticRows(sl_element_id_t parent_id, const std::wstring& query,
                                              size_t offset, size_t limit) const
      -> SearchResultPage;
  /// Install or clear (empty function) the query thread observer. Thread-safe.
  void               SetQueryThreadObserver(QueryThreadObserver observer);
  void               SetSemanticSearchProvider(std::shared_ptr<SemanticSearchProvider> provider);
  [[nodiscard]] auto HasSemanticSearchProvider() const -> bool;
  /// The embeddings of the trimmed @p query through the provider.
  /// @throws std::runtime_error when no provider is set, the query is empty, or the provider
  ///         fails.
  [[nodiscard]] auto EmbedSemanticQuery(const std::wstring& query) const -> SemanticQueryEmbedding;
  /// SemanticSearchProvider::IsPrewarmed and Prewarm of the provider; false and a no-op without
  /// a provider. Prewarm throws the provider error.
  [[nodiscard]] auto IsSemanticSearchPrewarmed() const -> bool;
  void               PrewarmSemanticSearch() const;
  /// One page of the files kept by the relevance rule, in relevance order, and the number of
  /// kept files. Empty when no provider is set or the query is empty.
  /// @throws std::runtime_error when the embedding or the ranking fails.
  [[nodiscard]] auto SearchFolderSemantic(sl_element_id_t parent_id, const std::wstring& query,
                                          size_t offset = 0, size_t limit = 48) const
      -> SemanticRankedPage;
  [[nodiscard]] auto CountSearchResults(sl_element_id_t parent_id, const std::wstring& query,
                                        SearchFieldMask mask = kAllSearchFields) const
      -> size_t;
  /// Invalidate all cached filter results for a specific folder scope.
  /// Call after membership changes (link / unlink / delete) that affect that folder.
  void InvalidateResultCache(sl_element_id_t folder_id);

  /// Invalidate the entire filter result cache.
  void InvalidateResultCache();

 private:
  void NotifyQueryThreadObserver(std::string_view operation) const;
};
}  // namespace alcedo
