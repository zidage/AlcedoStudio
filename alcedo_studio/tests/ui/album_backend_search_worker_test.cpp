//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Search off the UI thread (library_search_and_project_size_plan.md, Phase S5): every search
// route of SearchController runs its SQL on the search worker, the dialog receives only the
// newest response, and an applied search replaces the grid and stats from the worker result.
// A natural-language search applied to the library (global_search_redesign_plan.md, Phase GS2)
// shows the kept files in relevance order and embeds the query once. The palette data of
// SearchController (Phase GS3): recent searches and the recommendation rows.

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_model_registry.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"

namespace alcedo::ui::test {
namespace {

using SearchWorkerTests                    = ApplicationModuleHostTestFixture;

constexpr std::size_t kSyntheticImageCount = 12;

auto                  WaitUntil(const std::function<bool()>& predicate, int timeoutMs) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(10);
  }
  return predicate();
}

/// Records the thread of every search and stats query of one project's filter service. The
/// test hook of Phase S5 step 7: an entry on the UI thread fails the test.
class SearchQueryThreadRecorder {
 public:
  explicit SearchQueryThreadRecorder(std::shared_ptr<SleeveFilterService> filter_service)
      : filter_service_(std::move(filter_service)),
        ui_thread_(QCoreApplication::instance()->thread()) {
    filter_service_->SetQueryThreadObserver([this](std::string_view operation) {
      std::lock_guard lock(mutex_);
      ++query_count_;
      operations_.emplace_back(operation);
      if (QThread::currentThread() == ui_thread_) {
        ui_thread_operations_.emplace_back(operation);
      }
    });
  }
  ~SearchQueryThreadRecorder() { filter_service_->SetQueryThreadObserver({}); }

  SearchQueryThreadRecorder(const SearchQueryThreadRecorder&)            = delete;
  SearchQueryThreadRecorder& operator=(const SearchQueryThreadRecorder&) = delete;

  auto                       QueryCount() -> int {
    std::lock_guard lock(mutex_);
    return query_count_;
  }
  auto Operations() -> std::vector<std::string> {
    std::lock_guard lock(mutex_);
    return operations_;
  }
  auto UiThreadOperations() -> std::vector<std::string> {
    std::lock_guard lock(mutex_);
    return ui_thread_operations_;
  }

 private:
  std::shared_ptr<SleeveFilterService> filter_service_;
  QThread*                             ui_thread_ = nullptr;
  std::mutex                           mutex_;
  int                                  query_count_ = 0;
  std::vector<std::string>             operations_;
  std::vector<std::string>             ui_thread_operations_;
};

auto JoinOperations(const std::vector<std::string>& operations) -> std::string {
  std::string out;
  for (const auto& operation : operations) {
    out += operation + " ";
  }
  return out;
}

auto RowFileNames(const QVariantMap& response) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& row : response.value("rows").toList()) {
    names.push_back(row.toMap().value("fileName").toString().toStdString());
  }
  return names;
}

class LoadedPopulatedProject {
 public:
  auto Load(const std::filesystem::path& temp_dir, ApplicationModuleHost& backend,
            std::size_t image_count) -> bool {
    const auto packed_project = CreatePopulatedPackedProject(temp_dir, {}, image_count);
    if (!packed_project.has_value() || !LoadPackedProject(backend, packed_project->packed_path_)) {
      return false;
    }
    filter_service_ = backend.project()->handler().project()->GetSleeveFilterService();
    return filter_service_ != nullptr &&
           backend.library()->TotalCount() == static_cast<int>(image_count);
  }
  auto filter_service() const -> const std::shared_ptr<SleeveFilterService>& {
    return filter_service_;
  }

 private:
  std::shared_ptr<SleeveFilterService> filter_service_;
};

}  // namespace

TEST_F(SearchWorkerTests, PreviewRowsCarryDisplayColumnsAndTotalFromTheQuery) {
  ApplicationModuleHost backend;
  LoadedPopulatedProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto*      search = backend.search();

  QSignalSpy responses(search, &SearchController::SearchResponseReady);
  const auto request_id = search->RequestSearch(QStringLiteral("album-delete-1"), 0, 2);
  // The response is queued: the call returns before any result exists.
  EXPECT_EQ(responses.count(), 0);
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 1; }, 10000));

  const auto args = responses.takeFirst();
  EXPECT_EQ(args.at(0).toULongLong(), request_id);
  EXPECT_EQ(args.at(1).toString(), QStringLiteral("replace"));
  const auto response = args.at(2).toMap();
  // album-delete-1, album-delete-10, album-delete-11 match; the page holds the first two.
  EXPECT_EQ(response.value("total").toInt(), 3);
  EXPECT_TRUE(response.value("hasMore").toBool());
  EXPECT_EQ(response.value("route").toString(), QStringLiteral("traditional"));
  EXPECT_EQ(RowFileNames(response),
            (std::vector<std::string>{"album-delete-1.dng", "album-delete-10.dng"}));
  const auto row = response.value("rows").toList().front().toMap();
  EXPECT_EQ(row.value("cameraModel").toString(), QStringLiteral("Synthetic Album Camera"));
  EXPECT_EQ(row.value("lens").toString(), QStringLiteral("Synthetic 50mm"));
  EXPECT_EQ(row.value("captureDate").toString(), QStringLiteral("2026-05-25"));
  EXPECT_EQ(row.value("rating").toInt(), 0);
  EXPECT_NE(row.value("imageId").toUInt(), 0u);

  // Page 2 keeps the total and reports no more rows.
  search->RequestSearch(QStringLiteral("album-delete-1"), 2, 2, QStringLiteral("append"));
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 1; }, 10000));
  const auto page_two = responses.takeFirst().at(2).toMap();
  EXPECT_EQ(page_two.value("total").toInt(), 3);
  EXPECT_FALSE(page_two.value("hasMore").toBool());
  EXPECT_EQ(RowFileNames(page_two), (std::vector<std::string>{"album-delete-11.dng"}));
}

TEST_F(SearchWorkerTests, RapidPreviewRequestsDeliverOnlyTheNewestResponse) {
  ApplicationModuleHost backend;
  LoadedPopulatedProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto*         search = backend.search();

  // Twenty characters typed with no event loop turn in between: twenty requests.
  const QString query  = QStringLiteral("album-delete-11.dng ");
  ASSERT_EQ(query.size(), 20);
  QSignalSpy responses(search, &SearchController::SearchResponseReady);
  qulonglong last_request_id = 0;
  const auto burst_start     = std::chrono::steady_clock::now();
  for (int length = 1; length <= query.size(); ++length) {
    const auto request_id = search->RequestSearch(query.left(length), 0, 24);
    EXPECT_GT(request_id, last_request_id);
    last_request_id = request_id;
  }
  // UI thread time of one keystroke request (Phase S0 target: 2 ms, no SQL).
  const double ui_ms_per_request =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - burst_start)
          .count() /
      query.size();
  RecordProperty("ui_ms_per_request", std::to_string(ui_ms_per_request));
  std::cout << "[search worker] UI thread time per preview request: " << ui_ms_per_request
            << " ms\n";
  EXPECT_LT(ui_ms_per_request, 2.0);

  ASSERT_TRUE(WaitUntil([&]() { return responses.count() >= 1; }, 10000));
  // A stale response would arrive right after the first one; give it the time.
  ProcessEvents(300);
  ASSERT_EQ(responses.count(), 1);
  const auto args = responses.takeFirst();
  EXPECT_EQ(args.at(0).toULongLong(), last_request_id);
  EXPECT_EQ(RowFileNames(args.at(2).toMap()), (std::vector<std::string>{"album-delete-11.dng"}));
}

TEST_F(SearchWorkerTests, ApplyFuzzySearchQueriesOnTheWorkerAndKeepsInspectorFilters) {
  ApplicationModuleHost backend;
  LoadedPopulatedProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto* search = backend.search();
  auto* stats  = backend.stats();

  // An Inspector filter that the applied search keeps (it matches every created file).
  stats->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Synthetic Album Camera"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(stats->HasActiveFilter());

  SearchQueryThreadRecorder recorder(project.filter_service());
  search->ApplyFuzzySearch(QStringLiteral("album-delete-1"));
  // Nothing is installed until the worker result reaches the UI thread.
  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_EQ(backend.library()->TotalCount(), static_cast<int>(kSyntheticImageCount));

  ASSERT_TRUE(WaitUntil([&]() { return search->HasActiveSearchFilter(); }, 10000));
  EXPECT_EQ(search->active_search_query(), QStringLiteral("album-delete-1"));
  EXPECT_TRUE(stats->HasActiveFilter());
  EXPECT_EQ(backend.library()->TotalCount(), 3);
  EXPECT_EQ(backend.library()->ShownCount(), 3);
  EXPECT_EQ(stats->TotalPhotoCount(), 3);
  ASSERT_EQ(stats->CameraStats().size(), 1);
  EXPECT_EQ(stats->CameraStats().front().toMap().value("count").toInt(), 3);

  // The WHERE build on the filter service; the library read (grid page, total, groups, and
  // stats from one evaluation of the merged predicate) runs through AlbumBrowseService.
  EXPECT_EQ(recorder.Operations(), (std::vector<std::string>{"BuildFuzzySearchWhere"}))
      << JoinOperations(recorder.Operations());
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

TEST_F(SearchWorkerTests, ClearFuzzySearchDropsAnApplyStillOnTheWorker) {
  ApplicationModuleHost backend;
  LoadedPopulatedProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto* search = backend.search();

  // The clear runs before the apply result reaches the UI thread (it is queued behind this
  // test code), so the result must not install the search afterwards.
  search->ApplyFuzzySearch(QStringLiteral("album-delete-1"));
  search->ClearFuzzySearch();
  ProcessEvents(500);

  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_TRUE(search->active_search_query().isEmpty());
  EXPECT_EQ(backend.library()->TotalCount(), static_cast<int>(kSyntheticImageCount));
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), static_cast<int>(kSyntheticImageCount));

  // A folder change clears the search the same way.
  search->ApplyFuzzySearch(QStringLiteral("album-delete-1"));
  backend.folders()->SelectFolder(0);
  ProcessEvents(500);
  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_EQ(backend.library()->TotalCount(), static_cast<int>(kSyntheticImageCount));
}

TEST_F(SearchWorkerTests, SearchSqlNeverRunsOnTheUiThread) {
  ApplicationModuleHost backend;
  LoadedPopulatedProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto*                     search = backend.search();

  SearchQueryThreadRecorder recorder(project.filter_service());
  QSignalSpy                responses(search, &SearchController::SearchResponseReady);

  search->RequestSearch(QStringLiteral("album"), 0, 5);
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 1; }, 10000));
  search->RequestSearch(QStringLiteral("album"), 5, 5, QStringLiteral("append"));
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 2; }, 10000));
  search->RequestSubmitSearch(QStringLiteral("2026-05-25"), 0, 5);
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 3; }, 10000));
  EXPECT_EQ(responses.at(2).at(2).toMap().value("total").toInt(),
            static_cast<int>(kSyntheticImageCount));

  search->ApplyFuzzySearch(QStringLiteral("synthetic"));
  ASSERT_TRUE(WaitUntil([&]() { return search->HasActiveSearchFilter(); }, 10000));
  const auto exact_id = responses.at(0)
                            .at(2)
                            .toMap()
                            .value("rows")
                            .toList()
                            .front()
                            .toMap()
                            .value("elementId")
                            .toUInt();
  search->ApplyExactSearch(exact_id);
  ASSERT_TRUE(WaitUntil([&]() { return backend.library()->TotalCount() == 1; }, 10000));
  // Changing the field scope re-applies the active search on the worker.
  search->SetSearchFieldExifEnabled(false);
  ProcessEvents(300);
  search->SetSearchFieldExifEnabled(true);
  ProcessEvents(300);

  EXPECT_GE(recorder.QueryCount(), 6);
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

// ── Natural-language search in the library (global_search_redesign_plan.md, Phase GS2) ──────

namespace {

constexpr const char* kWorkerSemanticModel = "worker-relevance-model";
constexpr const char* kSemanticQuery       = "a quiet harbor at dawn";

auto                  AxisVector(size_t axis) -> std::vector<float> {
  std::vector<float> vector(kSemanticEmbeddingDim, 0.0F);
  vector.at(axis) = 1.0F;
  return vector;
}

/// Unit vector with cosine similarity @p similarity to axis 0 and no part on the neutral axis.
auto FileEmbeddingWithQuerySimilarity(size_t index, float similarity) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(0)         = similarity;
  embedding.at(1 + index) = std::sqrt(1.0F - (similarity * similarity));
  return embedding;
}

/// Embeds every query on axis 0 with the neutral text on the last axis, so the relevance of a
/// file is its similarity to axis 0. Counts the calls; a failing provider throws like a runtime
/// that does not start. Called on the query worker.
class FakeSemanticSearchProvider final : public SemanticSearchProvider {
 public:
  auto EmbedQuery(const std::wstring& /*query*/) -> SemanticQueryEmbedding override {
    ++calls_;
    if (fail_) {
      throw std::runtime_error("Semantic runtime failed to start.");
    }
    return SemanticQueryEmbedding{.model_key_ = kWorkerSemanticModel,
                                  .dimension_ = kSemanticEmbeddingDim,
                                  .query_     = AxisVector(0),
                                  .neutral_   = AxisVector(kSemanticEmbeddingDim - 1)};
  }

  /// Records the call; the neutral text counts as cached after the first prewarm.
  void Prewarm() override {
    ++prewarm_calls_;
    prewarmed_ = true;
  }
  auto              IsPrewarmed() -> bool override { return prewarmed_.load(); }

  std::atomic<int>  calls_{0};
  std::atomic<bool> fail_{false};
  std::atomic<int>  prewarm_calls_{0};
  std::atomic<bool> prewarmed_{false};
};

/// Points the default QSettings store at an INI file in @p dir, so the search settings of a
/// test (the natural-language switch, the recent searches) do not change the user's settings.
/// QSettings reads and writes nothing without an organization name, so the test sets one.
/// Construct it before ApplicationModuleHost.
class ScopedSearchSettings {
 public:
  explicit ScopedSearchSettings(const std::filesystem::path& dir)
      : organization_name_(QCoreApplication::organizationName()),
        application_name_(QCoreApplication::applicationName()) {
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoSearchWorkerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AlbumBackendSearchWorkerTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       PathToQString(dir / "search_settings"));
  }
  ~ScopedSearchSettings() {
    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QCoreApplication::setOrganizationName(organization_name_);
    QCoreApplication::setApplicationName(application_name_);
  }

  ScopedSearchSettings(const ScopedSearchSettings&)            = delete;
  ScopedSearchSettings& operator=(const ScopedSearchSettings&) = delete;

 private:
  QString organization_name_;
  QString application_name_;
};

/// A loaded project whose files have embeddings of an active model with the given query
/// similarities (index i is the file `album-delete-<i>.dng`), and a fake provider.
class SemanticLibraryProject {
 public:
  auto Load(const std::filesystem::path& temp_dir, ApplicationModuleHost& backend,
            const std::vector<float>& similarities, const std::vector<LibraryPhotoSpec>& specs = {})
      -> bool {
    const auto packed = CreatePopulatedPackedProject(temp_dir, {}, similarities.size(), specs);
    if (!packed.has_value() || !LoadPackedProject(backend, packed->packed_path_)) {
      return false;
    }
    auto project = backend.project()->handler().project();
    if (!project) {
      return false;
    }
    filter_service_     = project->GetSleeveFilterService();
    auto&       storage = *project->GetStorage();
    std::string error;
    if (!storage.GetSemanticModelRegistry().UpsertModel(
            SemanticModelRecord{.model_key_     = kWorkerSemanticModel,
                                .model_id_      = kWorkerSemanticModel,
                                .revision_      = "test-rev",
                                .embedding_dim_ = kSemanticEmbeddingDim},
            &error) ||
        !storage.GetSemanticModelRegistry().SetActiveModelKey(kWorkerSemanticModel, &error)) {
      ADD_FAILURE() << error;
      return false;
    }
    file_ids_.assign(similarities.size(), 0);
    for (const auto& row : storage.GetElementStore().ListFilesInFolder(0)) {
      const auto index = IndexOfFileName(row.file_name_);
      if (!index.has_value() || *index >= similarities.size()) {
        continue;
      }
      file_ids_[*index] = row.file_id_;
      if (!storage.GetSemanticEmbeddingStore().UpsertImageEmbedding(
              SemanticImageEmbeddingRecord{
                  .file_id_   = row.file_id_,
                  .image_id_  = row.image_id_,
                  .model_key_ = kWorkerSemanticModel,
                  .embedding_ = FileEmbeddingWithQuerySimilarity(*index, similarities[*index])},
              &error)) {
        ADD_FAILURE() << error;
        return false;
      }
    }
    provider_ = std::make_shared<FakeSemanticSearchProvider>();
    filter_service_->SetSemanticSearchProvider(provider_);
    backend.search()->SetNaturalLanguageSearchEnabled(true);
    return std::ranges::none_of(file_ids_, [](sl_element_id_t id) { return id == 0; });
  }

  /// File ids of the files with these indices, in this order.
  auto Ids(std::initializer_list<size_t> indices) const -> std::vector<sl_element_id_t> {
    std::vector<sl_element_id_t> ids;
    for (const auto index : indices) {
      ids.push_back(file_ids_.at(index));
    }
    return ids;
  }
  auto file_ids() const -> const std::vector<sl_element_id_t>& { return file_ids_; }
  auto provider() const -> FakeSemanticSearchProvider& { return *provider_; }
  auto filter_service() const -> const std::shared_ptr<SleeveFilterService>& {
    return filter_service_;
  }

 private:
  static auto IndexOfFileName(const std::string& name) -> std::optional<size_t> {
    constexpr std::string_view kPrefix = "album-delete-";
    if (!name.starts_with(kPrefix)) {
      return std::nullopt;
    }
    return static_cast<size_t>(std::stoul(name.substr(kPrefix.size())));
  }

  std::shared_ptr<SleeveFilterService>        filter_service_;
  std::shared_ptr<FakeSemanticSearchProvider> provider_;
  std::vector<sl_element_id_t>                file_ids_;
};

/// Query similarity of the twelve files: 3, 7, 1, 10, and 5 are kept (scope below 30 files, so
/// the similarity limit decides), in this relevance order.
const std::vector<float> kTwelveFileSimilarities{0.02F, 0.50F, 0.02F, 0.70F, 0.02F, 0.30F,
                                                 0.02F, 0.60F, 0.02F, 0.02F, 0.40F, 0.02F};

auto                     GridIds(ApplicationModuleHost& backend) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& item : backend.library()->model().items()) {
    ids.push_back(item.element_id);
  }
  return ids;
}

auto Sorted(std::vector<sl_element_id_t> ids) -> std::vector<sl_element_id_t> {
  std::sort(ids.begin(), ids.end());
  return ids;
}

auto WaitForSemanticSearch(ApplicationModuleHost& backend) -> bool {
  return WaitUntil(
             [&]() {
               return backend.search()->active_search_is_semantic() &&
                      !backend.library()->QueryUpdating();
             },
             15000) &&
         WaitForLibraryQuery(backend);
}

}  // namespace

TEST_F(SearchWorkerTests, SemanticApplyShowsKeptFilesInRelevanceOrder) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto*                     search = backend.search();

  SearchQueryThreadRecorder recorder(project.filter_service());
  EXPECT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  // Nothing is installed until the worker result reaches the UI thread.
  EXPECT_FALSE(search->active_search_is_semantic());
  ASSERT_TRUE(WaitForSemanticSearch(backend));

  EXPECT_EQ(GridIds(backend), project.Ids({3, 7, 1, 10, 5}));
  EXPECT_EQ(backend.library()->TotalCount(), 5);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 5);
  EXPECT_EQ(search->active_search_query(), QString::fromUtf8(kSemanticQuery));
  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_TRUE(backend.library()->QueryError().isEmpty());
  EXPECT_EQ(project.provider().calls_.load(), 1);
  // The embedding and the relevance select run on the query worker.
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

TEST_F(SearchWorkerTests, PrewarmEmbedsNeutralTextOnce) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto*                     search = backend.search();

  SearchQueryThreadRecorder recorder(project.filter_service());
  search->PrewarmNaturalLanguageSearch();
  // A second call while the first job is pending submits nothing.
  search->PrewarmNaturalLanguageSearch();
  ASSERT_TRUE(WaitUntil([&]() { return project.provider().prewarm_calls_.load() == 1; }, 5000));
  // Let the job's completion reach the UI thread, then prewarm again: the neutral text is
  // cached, so the job returns without a provider prewarm.
  (void)WaitUntil([]() { return false; }, 200);
  search->PrewarmNaturalLanguageSearch();
  (void)WaitUntil([]() { return false; }, 300);
  EXPECT_EQ(project.provider().prewarm_calls_.load(), 1);
  EXPECT_EQ(project.provider().calls_.load(), 0);
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

TEST_F(SearchWorkerTests, PrewarmDoesNothingWhenSwitchIsOff) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search = backend.search();
  search->SetNaturalLanguageSearchEnabled(false);

  search->PrewarmNaturalLanguageSearch();
  (void)WaitUntil([]() { return false; }, 300);
  EXPECT_EQ(project.provider().prewarm_calls_.load(), 0);
  EXPECT_EQ(project.provider().calls_.load(), 0);
}

TEST_F(SearchWorkerTests, SemanticApplyKeepsInspectorFilters) {
  ScopedSearchSettings          settings(temp_dir_);
  ApplicationModuleHost         backend;
  SemanticLibraryProject        project;
  std::vector<LibraryPhotoSpec> specs(12);
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].model_ = i % 2 == 0 ? "Even Camera" : "Odd Camera";
  }
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities, specs));
  auto* search = backend.search();
  auto* stats  = backend.stats();

  stats->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Odd Camera"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_EQ(backend.library()->TotalCount(), 6);

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  // File 10 is kept but has the even camera; the camera filter stays.
  EXPECT_TRUE(stats->HasActiveFilter());
  EXPECT_EQ(GridIds(backend), project.Ids({3, 7, 1, 5}));
  EXPECT_EQ(stats->TotalPhotoCount(), 4);
  ASSERT_EQ(stats->CameraStats().size(), 1);
  EXPECT_EQ(stats->CameraStats().front().toMap().value("count").toInt(), 4);
}

TEST_F(SearchWorkerTests, SemanticPageReadsDoNotCallTheProvider) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  // 321 scored files: 121 at similarity 0.6 and 200 at 0.02. Over this scope the z value of the
  // first group is about 1.28, so 121 files are kept: more than one library page of 120.
  std::vector<float>     similarities(321, 0.02F);
  std::fill_n(similarities.begin(), 121, 0.60F);
  ASSERT_TRUE(project.Load(temp_dir_, backend, similarities));
  auto* search  = backend.search();
  auto* library = backend.library();

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  ASSERT_EQ(library->TotalCount(), 121);
  ASSERT_EQ(library->ShownCount(), 120);
  ASSERT_EQ(project.provider().calls_.load(), 1);

  // The next page, select all, a position read, and a refresh use the accepted embedding.
  ASSERT_TRUE(library->LoadMoreThumbnails());
  ASSERT_TRUE(WaitUntil([&]() { return library->ShownCount() == 121; }, 10000));
  library->SelectAllInCurrentView();
  ASSERT_TRUE(WaitUntil([&]() { return library->selection()->SelectedCount() == 121; }, 10000));
  QSignalSpy positions(library, &LibraryModule::FocusPositionReady);
  library->RequestFocusPosition(static_cast<uint>(project.file_ids().at(120)));
  ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }, 10000));
  EXPECT_GE(positions.front().at(1).toLongLong(), 0);
  backend.stats()->RefreshStats();
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(library->TotalCount(), 121);
  EXPECT_TRUE(search->active_search_is_semantic());

  EXPECT_EQ(project.provider().calls_.load(), 1);
  // Every kept file appears once; the rest of the scope does not.
  auto shown = GridIds(backend);
  EXPECT_EQ(std::set<sl_element_id_t>(shown.begin(), shown.end()).size(), shown.size());
}

TEST_F(SearchWorkerTests, SemanticApplyFailureKeepsAcceptedContentAndReportsError) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search  = backend.search();
  auto* library = backend.library();

  // An accepted field search: album-delete-1, -10, and -11.
  search->ApplyFuzzySearch(QStringLiteral("album-delete-1"));
  ASSERT_TRUE(WaitUntil([&]() { return search->HasActiveSearchFilter(); }, 10000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto accepted_grid = GridIds(backend);
  ASSERT_EQ(accepted_grid.size(), 3u);

  project.provider().fail_ = true;
  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitUntil([&]() { return !library->QueryError().isEmpty(); }, 15000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  EXPECT_TRUE(library->QueryError().contains(QStringLiteral("Semantic runtime failed to start.")))
      << library->QueryError().toStdString();
  EXPECT_EQ(GridIds(backend), accepted_grid);
  EXPECT_EQ(search->active_search_query(), QStringLiteral("album-delete-1"));
  EXPECT_TRUE(search->HasActiveSearchFilter());
  EXPECT_FALSE(search->active_search_is_semantic());

  // The pending natural-language search stays for Retry.
  project.provider().fail_ = false;
  library->RetryLibraryQuery();
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  EXPECT_EQ(GridIds(backend), project.Ids({3, 7, 1, 10, 5}));
  EXPECT_TRUE(library->QueryError().isEmpty());
  EXPECT_FALSE(search->HasActiveSearchFilter());
}

TEST_F(SearchWorkerTests, ApplySemanticSearchRejectsTooLongQueryWithoutRefresh) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto*      search  = backend.search();
  auto*      library = backend.library();
  QSignalSpy query_state(library, &LibraryModule::QueryStateChanged);

  QString    too_long;
  for (int word = 0; word < 80; ++word) {
    too_long += QStringLiteral("harbor ");
  }
  EXPECT_FALSE(search->ApplySemanticSearch(too_long).isEmpty());
  // An empty query is not an error and changes nothing.
  EXPECT_TRUE(search->ApplySemanticSearch(QStringLiteral("   ")).isEmpty());
  // A field-search query is rejected while natural-language search is on.
  EXPECT_FALSE(search->ApplySemanticSearch(QStringLiteral("album-delete-1.dng")).isEmpty());
  // With natural-language search off, every query uses field search.
  search->SetNaturalLanguageSearchEnabled(false);
  EXPECT_FALSE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  search->SetNaturalLanguageSearchEnabled(true);
  // A project without a semantic search provider rejects the query.
  project.filter_service()->SetSemanticSearchProvider(nullptr);
  EXPECT_FALSE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());

  ProcessEvents(300);
  EXPECT_EQ(query_state.count(), 0);
  EXPECT_FALSE(library->QueryUpdating());
  EXPECT_EQ(library->TotalCount(), 12);
  EXPECT_TRUE(search->active_search_query().isEmpty());
  EXPECT_EQ(project.provider().calls_.load(), 0);
}

TEST_F(SearchWorkerTests, FolderChangeReevaluatesActiveSemanticSearch) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search  = backend.search();
  auto* folders = backend.folders();

  // Folder "sub" holds files 0, 1, 3, and 5; its child "inner" is empty.
  folders->CreateFolder(QStringLiteral("sub"));
  ProcessEvents(300);
  const uint sub_ui_id = FindFolderId(folders->Folders(), QStringLiteral("sub"));
  ASSERT_NE(sub_ui_id, 0u);
  folders->SelectFolder(sub_ui_id);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto sub_id = folders->CurrentFolderElementId();
  ASSERT_TRUE(sub_id.has_value());
  auto       browse = backend.project()->handler().project()->GetAlbumBrowseService();
  const auto linked =
      browse->LinkFilesToFolder(project.Ids({0, 1, 3, 5}), static_cast<sl_element_id_t>(*sub_id));
  ASSERT_TRUE(linked.failed_element_ids_.empty());
  folders->CreateFolder(QStringLiteral("inner"));
  ProcessEvents(300);
  const uint inner_ui_id = FindFolderId(folders->Folders(), QStringLiteral("inner"));
  ASSERT_NE(inner_ui_id, 0u);
  folders->SelectFolder(inner_ui_id);
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  EXPECT_EQ(backend.library()->TotalCount(), 0);
  ASSERT_EQ(project.provider().calls_.load(), 1);

  // Deleting the current folder selects its parent and keeps the search: the parent shows its
  // kept files (3, 1, 5) in relevance order from the accepted embedding.
  folders->DeleteFolder(inner_ui_id);
  ASSERT_TRUE(WaitUntil([&]() { return folders->CurrentFolderElementId() == sub_id; }, 10000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(search->active_search_is_semantic());
  EXPECT_EQ(GridIds(backend), project.Ids({3, 1, 5}));
  EXPECT_EQ(project.provider().calls_.load(), 1);

  // Selecting a folder clears the search, as for field search.
  const uint reloaded_sub_ui_id = FindFolderId(folders->Folders(), QStringLiteral("sub"));
  ASSERT_NE(reloaded_sub_ui_id, 0u);
  folders->SelectFolder(reloaded_sub_ui_id);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_FALSE(search->active_search_is_semantic());
  EXPECT_EQ(GridIds(backend), Sorted(project.Ids({0, 1, 3, 5})));
}

TEST_F(SearchWorkerTests, ClearSearchRemovesSemanticOrder) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search = backend.search();

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  ASSERT_EQ(GridIds(backend), project.Ids({3, 7, 1, 10, 5}));

  search->ClearFuzzySearch();
  EXPECT_FALSE(search->active_search_is_semantic());
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(search->active_search_query().isEmpty());
  EXPECT_EQ(GridIds(backend), Sorted(project.file_ids()));
  EXPECT_EQ(backend.library()->TotalCount(), 12);
  EXPECT_EQ(project.provider().calls_.load(), 1);
}

TEST_F(SearchWorkerTests, NonsenseSemanticQueryShowsNoPhotos) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  // No file reaches the similarity limit.
  ASSERT_TRUE(project.Load(temp_dir_, backend, std::vector<float>(12, 0.02F)));
  auto* search = backend.search();

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  EXPECT_EQ(backend.library()->TotalCount(), 0);
  EXPECT_TRUE(GridIds(backend).empty());
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);
  EXPECT_TRUE(backend.library()->QueryError().isEmpty());
  EXPECT_EQ(search->active_search_query(), QString::fromUtf8(kSemanticQuery));
}

// ── Palette data in SearchController (global_search_redesign_plan.md, Phase GS3) ────────────

namespace {

using RecentPairList = std::vector<std::pair<std::string, std::string>>;

/// Recent searches as (query, route) pairs, newest first.
auto RecentPairs(const SearchController& search) -> RecentPairList {
  RecentPairList pairs;
  for (const auto& value : search.RecentSearches()) {
    const auto entry = value.toMap();
    pairs.emplace_back(entry.value("query").toString().toStdString(),
                       entry.value("route").toString().toStdString());
  }
  return pairs;
}

/// Apply a field search and wait until the library accepted it.
auto ApplyFuzzyAndWait(ApplicationModuleHost& backend, const QString& query) -> bool {
  backend.search()->ApplyFuzzySearch(query);
  return WaitUntil(
             [&]() {
               return backend.search()->active_search_query() == query &&
                      !backend.library()->QueryUpdating();
             },
             10000) &&
         WaitForLibraryQuery(backend);
}

}  // namespace

TEST_F(SearchWorkerTests, RecentSearchAddsAcceptedQueryAtTheTop) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search = backend.search();
  ASSERT_TRUE(search->RecentSearches().isEmpty());

  QSignalSpy recent_changes(search, &SearchController::RecentSearchesChanged);
  ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-1")));
  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));

  EXPECT_EQ(RecentPairs(*search),
            (RecentPairList{{kSemanticQuery, "semantic"}, {"album-delete-1", "fuzzy"}}));
  EXPECT_EQ(recent_changes.count(), 2);
}

TEST_F(SearchWorkerTests, RecentSearchMovesARepeatedQueryWithoutDuplicate) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  LoadedPopulatedProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto* search = backend.search();

  ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-1")));
  ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-2")));
  ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-1")));

  EXPECT_EQ(RecentPairs(*search),
            (RecentPairList{{"album-delete-1", "fuzzy"}, {"album-delete-2", "fuzzy"}}));
}

TEST_F(SearchWorkerTests, RecentSearchKeepsAtMostFiveEntries) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  LoadedPopulatedProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto* search = backend.search();

  for (int index = 1; index <= 6; ++index) {
    ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-%1").arg(index)));
  }

  ASSERT_EQ(search->RecentSearches().size(), SearchController::kMaxRecentSearches);
  EXPECT_EQ(RecentPairs(*search), (RecentPairList{{"album-delete-6", "fuzzy"},
                                                  {"album-delete-5", "fuzzy"},
                                                  {"album-delete-4", "fuzzy"},
                                                  {"album-delete-3", "fuzzy"},
                                                  {"album-delete-2", "fuzzy"}}));
}

TEST_F(SearchWorkerTests, ExactFileSearchIsNotARecentSearch) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto*      search = backend.search();
  QSignalSpy recent_changes(search, &SearchController::RecentSearchesChanged);

  search->ApplyExactSearch(static_cast<uint>(project.file_ids().at(3)));
  ASSERT_TRUE(WaitUntil([&]() { return backend.library()->TotalCount() == 1; }, 10000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_FALSE(search->active_search_query().isEmpty());

  EXPECT_TRUE(search->RecentSearches().isEmpty());
  EXPECT_EQ(recent_changes.count(), 0);
}

TEST_F(SearchWorkerTests, FailedSearchIsNotARecentSearch) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search             = backend.search();
  auto* library            = backend.library();

  project.provider().fail_ = true;
  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitUntil([&]() { return !library->QueryError().isEmpty(); }, 15000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  EXPECT_FALSE(search->active_search_is_semantic());
  EXPECT_TRUE(search->RecentSearches().isEmpty());
}

TEST_F(SearchWorkerTests, RecentSearchesPersistInSettings) {
  ScopedSearchSettings settings(temp_dir_);
  {
    ApplicationModuleHost  backend;
    SemanticLibraryProject project;
    ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
    ASSERT_TRUE(ApplyFuzzyAndWait(backend, QStringLiteral("album-delete-1")));
    ASSERT_TRUE(backend.search()->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
    ASSERT_TRUE(WaitForSemanticSearch(backend));
  }

  const RecentPairList   expected{{kSemanticQuery, "semantic"}, {"album-delete-1", "fuzzy"}};
  // The constructor reads the list; it needs no project.
  const SearchController reader(nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(RecentPairs(reader), expected);

  // An entry with an unknown route is skipped on read.
  QVariantList stored = QSettings{}.value(QStringLiteral("search/recentQueries")).toList();
  ASSERT_EQ(stored.size(), 2);
  stored.prepend(
      QVariantMap{{"query", QStringLiteral("album-delete-9")}, {"route", QStringLiteral("exact")}});
  QSettings{}.setValue(QStringLiteral("search/recentQueries"), stored);
  const SearchController second_reader(nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(RecentPairs(second_reader), expected);
}

TEST_F(SearchWorkerTests, RecentSemanticSearchUsesItsStoredRoute) {
  ScopedSearchSettings   settings(temp_dir_);
  ApplicationModuleHost  backend;
  SemanticLibraryProject project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities));
  auto* search = backend.search();

  ASSERT_TRUE(search->ApplySemanticSearch(QString::fromUtf8(kSemanticQuery)).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));
  search->ClearFuzzySearch();
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_FALSE(search->active_search_is_semantic());

  // With the switch off, the query text uses field search; the stored entry keeps its route.
  search->SetNaturalLanguageSearchEnabled(false);
  ASSERT_EQ(search->session()->RouteFor(QString::fromUtf8(kSemanticQuery)),
            QStringLiteral("fuzzy"));
  EXPECT_FALSE(search->ApplyRecentSearch(1).isEmpty());
  EXPECT_FALSE(search->ApplyRecentSearch(-1).isEmpty());
  EXPECT_TRUE(search->ApplyRecentSearch(0).isEmpty());
  ASSERT_TRUE(WaitForSemanticSearch(backend));

  EXPECT_EQ(GridIds(backend), project.Ids({3, 7, 1, 10, 5}));
  EXPECT_EQ(search->active_search_query(), QString::fromUtf8(kSemanticQuery));
  EXPECT_EQ(RecentPairs(*search), (RecentPairList{{kSemanticQuery, "semantic"}}));
}

TEST_F(SearchWorkerTests, RecommendationAppliesStatsFilter) {
  ScopedSearchSettings          settings(temp_dir_);
  ApplicationModuleHost         backend;
  SemanticLibraryProject        project;
  std::vector<LibraryPhotoSpec> specs(12);
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].model_ = i < 4 ? "Canon EOS R5" : "Nikon Z8";
  }
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities, specs));
  auto*      search  = backend.search();
  auto*      stats   = backend.stats();
  auto*      library = backend.library();
  QSignalSpy filter_changes(stats, &StatsEngine::StatsFilterChanged);

  search->ApplyRecommendation(QStringLiteral("camera"), QStringLiteral("Canon EOS R5"));
  EXPECT_EQ(stats->filter_camera(), QStringLiteral("Canon EOS R5"));
  EXPECT_TRUE(library->QueryUpdating());
  EXPECT_EQ(filter_changes.count(), 1);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(library->TotalCount(), 4);
  EXPECT_TRUE(search->active_search_query().isEmpty());

  // A second click on the same row keeps the filter and starts no refresh.
  filter_changes.clear();
  search->ApplyRecommendation(QStringLiteral("camera"), QStringLiteral("Canon EOS R5"));
  EXPECT_EQ(stats->filter_camera(), QStringLiteral("Canon EOS R5"));
  EXPECT_FALSE(library->QueryUpdating());
  EXPECT_EQ(filter_changes.count(), 0);
  EXPECT_EQ(library->TotalCount(), 4);
}

TEST_F(SearchWorkerTests, MatchingRecommendationsFilterByLabel) {
  ScopedSearchSettings          settings(temp_dir_);
  ApplicationModuleHost         backend;
  SemanticLibraryProject        project;
  std::vector<LibraryPhotoSpec> specs(12);
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].model_ = i < 4 ? "Canon EOS R5" : "Nikon Z8";
    specs[i].lens_  = i < 4 ? "RF 24-70mm F2.8" : "Z 50mm F1.8";
  }
  ASSERT_TRUE(project.Load(temp_dir_, backend, kTwelveFileSimilarities, specs));
  auto*      search = backend.search();

  const auto rows   = search->MatchingRecommendations(QStringLiteral("r5"), 4);
  ASSERT_EQ(rows.size(), 1);
  const auto row = rows.front().toMap();
  EXPECT_EQ(row.value("category").toString(), QStringLiteral("camera"));
  EXPECT_EQ(row.value("label").toString(), QStringLiteral("Canon EOS R5"));
  EXPECT_EQ(row.value("count").toInt(), 4);

  // The query matches inside a label of any category, ignores case, and the limit applies.
  const auto lens_rows = search->MatchingRecommendations(QStringLiteral("50MM"), 4);
  ASSERT_EQ(lens_rows.size(), 1);
  EXPECT_EQ(lens_rows.front().toMap().value("category").toString(), QStringLiteral("lens"));
  EXPECT_EQ(search->MatchingRecommendations(QStringLiteral("o"), 1).size(), 1);
  EXPECT_TRUE(search->MatchingRecommendations(QStringLiteral("   "), 4).isEmpty());
  EXPECT_TRUE(search->MatchingRecommendations(QStringLiteral("sony"), 4).isEmpty());
}

TEST_F(SearchWorkerTests, SearchSettingsPanelOpenStatePersistsInSettings) {
  ScopedSearchSettings settings(temp_dir_);
  {
    SearchController controller(nullptr, nullptr, nullptr, nullptr);
    EXPECT_FALSE(controller.SearchSettingsPanelOpen());
    QSignalSpy changes(&controller, &SearchController::SearchSettingsPanelOpenChanged);
    controller.SetSearchSettingsPanelOpen(true);
    controller.SetSearchSettingsPanelOpen(true);
    EXPECT_EQ(changes.count(), 1);
  }
  const SearchController reader(nullptr, nullptr, nullptr, nullptr);
  EXPECT_TRUE(reader.SearchSettingsPanelOpen());
  EXPECT_TRUE(QSettings{}.value(QStringLiteral("search/settingsPanelOpen")).toBool());
}

}  // namespace alcedo::ui::test
