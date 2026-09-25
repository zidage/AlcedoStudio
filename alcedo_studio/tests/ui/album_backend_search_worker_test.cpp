//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Search off the UI thread (library_search_and_project_size_plan.md, Phase S5): every search
// route of SearchController runs its SQL on the search worker, the dialog receives only the
// newest response, and an applied search replaces the grid and stats from the worker result.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "ui/album_backend_seeded_project_fixture.hpp"
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
  auto UiThreadOperations() -> std::vector<std::string> {
    std::lock_guard lock(mutex_);
    return ui_thread_operations_;
  }

 private:
  std::shared_ptr<SleeveFilterService> filter_service_;
  QThread*                             ui_thread_ = nullptr;
  std::mutex                           mutex_;
  int                                  query_count_ = 0;
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

class LoadedSeededProject {
 public:
  auto Load(const std::filesystem::path& temp_dir, ApplicationModuleHost& backend,
            std::size_t image_count) -> bool {
    const auto packed_project = CreateSeededPackedProject(temp_dir, {}, image_count);
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
  LoadedSeededProject   project;
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
  LoadedSeededProject   project;
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

TEST_F(SearchWorkerTests, ApplyFuzzySearchQueriesOnTheWorkerAndCommitsGridAndStats) {
  ApplicationModuleHost backend;
  LoadedSeededProject   project;
  ASSERT_TRUE(project.Load(temp_dir_, backend, kSyntheticImageCount));
  auto* search = backend.search();
  auto* stats  = backend.stats();

  // A stats filter that the applied search clears.
  stats->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Synthetic Album Camera"));
  ASSERT_TRUE(stats->HasActiveFilter());

  SearchQueryThreadRecorder recorder(project.filter_service());
  search->ApplyFuzzySearch(QStringLiteral("album-delete-1"));
  // Nothing is installed until the worker result reaches the UI thread.
  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_EQ(backend.library()->TotalCount(), static_cast<int>(kSyntheticImageCount));

  ASSERT_TRUE(WaitUntil([&]() { return search->HasActiveSearchFilter(); }, 10000));
  EXPECT_EQ(search->active_search_query(), QStringLiteral("album-delete-1"));
  EXPECT_FALSE(stats->HasActiveFilter());
  EXPECT_EQ(backend.library()->TotalCount(), 3);
  EXPECT_EQ(backend.library()->ShownCount(), 3);
  EXPECT_EQ(stats->TotalPhotoCount(), 3);
  ASSERT_EQ(stats->CameraStats().size(), 1);
  EXPECT_EQ(stats->CameraStats().front().toMap().value("count").toInt(), 3);

  EXPECT_GE(recorder.QueryCount(), 3);  // WHERE build, grid page, stats.
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

TEST_F(SearchWorkerTests, ClearFuzzySearchDropsAnApplyStillOnTheWorker) {
  ApplicationModuleHost backend;
  LoadedSeededProject   project;
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
  LoadedSeededProject   project;
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

  EXPECT_GE(recorder.QueryCount(), 9);
  EXPECT_TRUE(recorder.UiThreadOperations().empty())
      << JoinOperations(recorder.UiThreadOperations());
}

}  // namespace alcedo::ui::test
