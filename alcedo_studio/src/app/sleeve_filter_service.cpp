//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_filter_service.hpp"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/search_query_parser.hpp"
#include "storage/query/filter_compiler.hpp"

namespace alcedo {
namespace {
auto FilterScopeCacheKey(filter_id_t filter_id, sl_element_id_t parent_id) -> std::uint64_t {
  return (static_cast<std::uint64_t>(parent_id) << 32U) | static_cast<std::uint64_t>(filter_id);
}

auto TrimCopy(std::wstring value) -> std::wstring {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](wchar_t ch) { return std::iswspace(ch) != 0; });
  const auto last  = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
                      return std::iswspace(ch) != 0;
                    }).base();
  if (first >= last) {
    return {};
  }
  return std::wstring(first, last);
}

/// A LibrarySearch filter node of @p request and @p capabilities.
auto LibrarySearchNode(LibrarySearchRequest request, SearchCapabilities capabilities)
    -> FilterNode {
  FilterNode node{FilterNode::Type::LibrarySearch, FilterOp::AND, {}, std::nullopt};
  node.library_search_ = LibrarySearchCondition{.request      = std::move(request),
                                                .capabilities = std::move(capabilities)};
  return node;
}

auto ToStatsBuckets(const std::vector<StorageStatsBucket>& buckets) -> std::vector<StatsBucket> {
  std::vector<StatsBucket> out;
  out.reserve(buckets.size());
  for (const auto& bucket : buckets) {
    out.push_back({bucket.label_, bucket.count_});
  }
  return out;
}

}  // namespace

auto ToAlbumStatsView(const FolderStatsView& stats) -> AlbumStatsView {
  return {.total_photo_count_ = stats.total_photo_count_,
          .date_stats_        = ToStatsBuckets(stats.date_stats_),
          .camera_stats_      = ToStatsBuckets(stats.camera_stats_),
          .lens_stats_        = ToStatsBuckets(stats.lens_stats_),
          .label_stats_       = ToStatsBuckets(stats.label_stats_),
          .rating_stats_      = ToStatsBuckets(stats.rating_stats_),
          .import_date_stats_ = ToStatsBuckets(stats.import_date_stats_),
          .color_label_counts_ = stats.color_label_counts_,
          .delete_mark_count_  = stats.delete_mark_count_};
}

auto SleeveFilterService::CreateFilterCombo(const FilterNode& root) -> filter_id_t {
  filter_id_t new_id = filter_id_generator_.GenerateID();
  filter_storage_.RecordAccess(new_id, std::make_shared<FilterCombo>(new_id, root));
  return new_id;
}

auto SleeveFilterService::GetFilterCombo(filter_id_t filter_id)
    -> std::optional<std::shared_ptr<FilterCombo>> {
  auto combo_opt = filter_storage_.AccessElement(filter_id);
  if (combo_opt.has_value()) {
    return combo_opt.value();
  } else {
    return std::nullopt;
  }
}

void SleeveFilterService::RemoveFilterCombo(filter_id_t filter_id) {
  // If there is no record, this is a no-op.
  filter_storage_.RemoveRecord(filter_id);
  // Result cache keys include folder scope; flushing keeps removal simple and stable.
  filter_result_cache_.Flush();
}

auto SleeveFilterService::ApplyFilterOn(filter_id_t filter_id, sl_element_id_t parent_id)
    -> std::optional<std::vector<sl_element_id_t>> {
  // First, check if the filter combo exists.
  auto combo_opt = filter_storage_.AccessElement(filter_id);
  if (!combo_opt.has_value()) {
    return std::nullopt;
  }
  auto       combo      = combo_opt.value();

  // Next, check if we have a cached result for this filter in this folder scope.
  const auto cache_key  = FilterScopeCacheKey(filter_id, parent_id);
  auto       result_opt = filter_result_cache_.AccessElement(cache_key);
  if (result_opt.has_value()) {
    return result_opt;
  }

  // No cached result, we need to execute the filter.
  auto result_ids =
      storage_->GetElementStore().GetElementIdsInFolderByFilter(combo, parent_id);
  // Cache the result for future use.
  filter_result_cache_.RecordAccess(cache_key, result_ids);
  return result_ids;
}

auto SleeveFilterService::BuildFolderStats(sl_element_id_t                  parent_id,
                                           const std::optional<FilterNode>& extra_filter) const
    -> AlbumStatsView {
  NotifyQueryThreadObserver("BuildFolderStats");
  return ToAlbumStatsView(storage_->GetElementStore().BuildFolderStats(
      parent_id, CompileFilterPredicate(extra_filter),
      storage_->GetSemanticModelRegistry().ActiveModelKey()));
}

auto SleeveFilterService::ListSearchResultPageWithStats(sl_element_id_t                  parent_id,
                                                        const std::optional<FilterNode>& filter,
                                                        size_t offset, size_t limit) const
    -> SearchResultPageAndStats {
  NotifyQueryThreadObserver("ListSearchResultPageWithStats");
  if (!storage_) {
    return {};
  }
  auto result = storage_->GetElementStore().ListSearchResultPageWithStats(
      parent_id, offset, limit, CompileFilterPredicate(filter),
      storage_->GetSemanticModelRegistry().ActiveModelKey());
  return {.page_ = std::move(result.page_), .stats_ = ToAlbumStatsView(result.stats_)};
}

auto SleeveFilterService::BuildFuzzySearchWhere(const std::wstring& query,
                                                SearchFieldMask      mask) const
    -> std::optional<FilterNode> {
  NotifyQueryThreadObserver("BuildFuzzySearchWhere");
  const auto trimmed = TrimCopy(query);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (mask == 0) {
    // No field selected: nothing can match. Distinct from nullopt (which would mean "no
    // filter", i.e. match everything). The predicate of a request without fields is FALSE.
    return LibrarySearchNode(LibrarySearchRequest{.query_text = trimmed, .terms = {}, .fields = 0},
                             {});
  }

  auto terms = SearchQueryParser::Parse(trimmed);
  if (terms.empty()) {
    return std::nullopt;
  }

  // Both values are held in memory by their stores, so building the node runs no SQL.
  SearchCapabilities capabilities;
  if (storage_) {
    capabilities.semantic_model_key = storage_->GetSemanticModelRegistry().ActiveModelKey();
    capabilities.has_ai_fts_index   = storage_->GetAiStore().HasUnderstandingFtsIndex();
  }
  return LibrarySearchNode(
      LibrarySearchRequest{.query_text = trimmed, .terms = std::move(terms), .fields = mask},
      std::move(capabilities));
}

auto SleeveFilterService::BuildExactFileWhere(sl_element_id_t file_id) const -> FilterNode {
  return FilterNode{FilterNode::Type::Condition,
                    FilterOp::AND,
                    {},
                    FieldCondition{.field_ = FilterField::FileId,
                                   .op_    = CompareOp::EQUALS,
                                   .value_ = static_cast<int64_t>(file_id)}};
}

auto SleeveFilterService::SearchFolder(sl_element_id_t parent_id, const std::wstring& query,
                                       size_t offset, size_t limit, SearchFieldMask mask) const
    -> std::vector<FuzzySearchMatch> {
  NotifyQueryThreadObserver("SearchFolder");
  std::vector<FuzzySearchMatch> out;
  if (!storage_) {
    return out;
  }
  const auto filter_node = BuildFuzzySearchWhere(query, mask);
  if (!filter_node.has_value()) {
    return out;
  }
  const auto where = CompileFilterPredicate(filter_node);
  if (!where.has_value()) {
    return out;
  }

  const auto rows = storage_->GetElementStore().ListFilesInFolderPage(
      parent_id, offset, limit, where);
  out.reserve(rows.size());
  for (const auto& row : rows) {
    out.push_back({row.file_id_, row.image_id_, row.file_name_});
  }
  return out;
}

void SleeveFilterService::SetSemanticSearchProvider(
    std::shared_ptr<SemanticSearchProvider> provider) {
  semantic_search_provider_ = std::move(provider);
}

auto SleeveFilterService::HasSemanticSearchProvider() const -> bool {
  return semantic_search_provider_ != nullptr;
}

auto SleeveFilterService::EmbedSemanticQuery(const std::wstring& query) const
    -> SemanticQueryEmbedding {
  if (!semantic_search_provider_) {
    throw std::runtime_error("No semantic search provider is available.");
  }
  const auto text = TrimCopy(query);
  if (text.empty()) {
    throw std::runtime_error("Semantic search query is empty.");
  }
  return semantic_search_provider_->EmbedQuery(text);
}

auto SleeveFilterService::IsSemanticSearchPrewarmed() const -> bool {
  return semantic_search_provider_ && semantic_search_provider_->IsPrewarmed();
}

void SleeveFilterService::PrewarmSemanticSearch() const {
  if (semantic_search_provider_) {
    semantic_search_provider_->Prewarm();
  }
}

auto SleeveFilterService::SearchFolderSemantic(sl_element_id_t parent_id, const std::wstring& query,
                                               size_t offset, size_t limit) const
    -> SemanticRankedPage {
  NotifyQueryThreadObserver("SearchFolderSemantic");
  if (!semantic_search_provider_ || !storage_ || TrimCopy(query).empty()) {
    return {};
  }
  const auto  embedding = EmbedSemanticQuery(query);
  std::string error;
  auto        page =
      storage_->GetSemanticVectorSearch().SearchImageEmbeddings(parent_id, embedding, offset,
                                                                limit, &error);
  if (!error.empty()) {
    throw std::runtime_error(error);
  }
  return page;
}

auto SleeveFilterService::SearchFolderPage(sl_element_id_t parent_id, const std::wstring& query,
                                           size_t offset, size_t limit, SearchFieldMask mask) const
    -> SearchResultPage {
  NotifyQueryThreadObserver("SearchFolderPage");
  if (!storage_) {
    return {};
  }
  const auto filter_node = BuildFuzzySearchWhere(query, mask);
  if (!filter_node.has_value()) {
    return {};
  }
  return ListSearchResultPage(parent_id, filter_node, offset, limit);
}

auto SleeveFilterService::ListSearchResultPage(sl_element_id_t                  parent_id,
                                               const std::optional<FilterNode>& filter,
                                               size_t offset, size_t limit) const
    -> SearchResultPage {
  NotifyQueryThreadObserver("ListSearchResultPage");
  if (!storage_) {
    return {};
  }
  return storage_->GetElementStore().ListSearchResultPage(parent_id, offset, limit,
                                                          CompileFilterPredicate(filter));
}

auto SleeveFilterService::SearchFolderSemanticRows(sl_element_id_t     parent_id,
                                                   const std::wstring& query, size_t offset,
                                                   size_t limit) const -> SearchResultPage {
  NotifyQueryThreadObserver("SearchFolderSemanticRows");
  if (!storage_) {
    return {};
  }
  const auto                   ranked = SearchFolderSemantic(parent_id, query, offset, limit);
  std::vector<sl_element_id_t> file_ids;
  file_ids.reserve(ranked.rows_.size());
  for (const auto& row : ranked.rows_) {
    file_ids.push_back(row.file_id_);
  }
  return SearchResultPage{.total_ = ranked.total_,
                          .rows_  = storage_->GetElementStore().ListSearchResultRows(file_ids)};
}

void SleeveFilterService::SetQueryThreadObserver(QueryThreadObserver observer) {
  std::lock_guard lock(query_thread_observer_mutex_);
  query_thread_observer_ = std::move(observer);
}

void SleeveFilterService::NotifyQueryThreadObserver(std::string_view operation) const {
  QueryThreadObserver observer;
  {
    std::lock_guard lock(query_thread_observer_mutex_);
    observer = query_thread_observer_;
  }
  if (observer) {
    observer(operation);
  }
}

auto SleeveFilterService::CountSearchResults(sl_element_id_t     parent_id,
                                             const std::wstring& query,
                                             SearchFieldMask      mask) const -> size_t {
  NotifyQueryThreadObserver("CountSearchResults");
  if (!storage_) {
    return 0;
  }
  const auto filter_node = BuildFuzzySearchWhere(query, mask);
  if (!filter_node.has_value()) {
    return 0;
  }
  const auto where = CompileFilterPredicate(filter_node);
  if (!where.has_value()) {
    return 0;
  }
  return storage_->GetElementStore().CountFilesInFolder(parent_id, where);
}

void SleeveFilterService::InvalidateResultCache(sl_element_id_t folder_id) {
  const auto keys = filter_result_cache_.GetLRUKeys();
  for (const auto& key : keys) {
    const auto key_folder_id = static_cast<sl_element_id_t>(key >> 32U);
    if (key_folder_id == folder_id) {
      filter_result_cache_.RemoveRecord(key);
    }
  }
}

void SleeveFilterService::InvalidateResultCache() { filter_result_cache_.Flush(); }
}  // namespace alcedo
