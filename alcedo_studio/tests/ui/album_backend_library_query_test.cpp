//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// One library query path for Inspector filters, search, sort, groups, and pages
// (album_sort_group_and_import_time_plan.md, Phase 2). Every case loads a persisted multi-photo
// project into the real ApplicationModuleHost; worker timing is held through the production
// query observer of AlbumBrowseService.

#include <QCoreApplication>
#include <QDateTime>
#include <QSignalSpy>
#include <QThread>
#include <QTimeZone>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"

namespace alcedo::ui::test {
namespace {

using LibraryQueryTests = ApplicationModuleHostTestFixture;

auto Utc(int year, unsigned month, unsigned day, int hour = 0) -> std::time_t {
  using namespace std::chrono;
  return static_cast<std::time_t>(
      (sys_days{std::chrono::year{year} / month / day} + hours{hour}).time_since_epoch().count());
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs = 15000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(10);
  }
  return predicate();
}

/// Six photos: three cameras, equal ratings, one unknown capture date, distinct import days.
auto SixPhotoSpecs() -> std::vector<LibraryPhotoSpec> {
  return {
      {.model_      = "Canon R5",
       .date_time_  = "2026-06-07 09:00:00",
       .rating_     = 3,
       .added_time_ = Utc(2026, 9, 1, 10)},
      {.model_      = "Canon R5",
       .date_time_  = "2026-06-07 18:00:00",
       .rating_     = 5,
       .added_time_ = Utc(2026, 9, 1, 11)},
      {.model_      = "Nikon Z8",
       .date_time_  = "2026-06-06 10:00:00",
       .rating_     = 3,
       .added_time_ = Utc(2026, 9, 2, 10)},
      {.model_      = "Nikon Z8",
       .date_time_  = "2026-06-06 11:00:00",
       .rating_     = 1,
       .added_time_ = Utc(2026, 9, 2, 11)},
      {.model_      = "Sony A7",
       .date_time_  = "2026-06-05 12:00:00",
       .rating_     = 5,
       .added_time_ = Utc(2026, 8, 30, 10)},
      {.model_ = "Sony A7", .date_time_ = "", .rating_ = 0, .added_time_ = Utc(2026, 8, 30, 11)},
  };
}

/// Holds the query worker inside AlbumBrowseService's observer, on the real worker thread.
class WorkerHold {
 public:
  explicit WorkerHold(std::shared_ptr<AlbumBrowseService> browse)
      : browse_(std::move(browse)), ui_thread_(QCoreApplication::instance()->thread()) {
    browse_->SetQueryThreadObserver([this](std::string_view operation) {
      std::unique_lock lock(mutex_);
      operations_.emplace_back(operation);
      if (QThread::currentThread() == ui_thread_) {
        ui_thread_operations_.emplace_back(operation);
      }
      if (hold_next_) {
        hold_next_ = false;
        held_      = true;
        changed_.notify_all();
        changed_.wait(lock, [this]() { return !held_; });
      }
    });
  }
  ~WorkerHold() {
    Release();
    browse_->SetQueryThreadObserver({});
  }
  WorkerHold(const WorkerHold&)            = delete;
  WorkerHold& operator=(const WorkerHold&) = delete;

  void        ArmNext() {
    std::lock_guard lock(mutex_);
    hold_next_ = true;
  }
  /// Process events until the worker waits in the observer.
  auto WaitHeld() -> bool {
    return WaitUntil([this]() {
      std::lock_guard lock(mutex_);
      return held_;
    });
  }
  void Release() {
    {
      std::lock_guard lock(mutex_);
      held_ = false;
    }
    changed_.notify_all();
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
  std::shared_ptr<AlbumBrowseService> browse_;
  QThread*                            ui_thread_ = nullptr;
  std::mutex                          mutex_;
  std::condition_variable             changed_;
  bool                                hold_next_ = false;
  bool                                held_      = false;
  std::vector<std::string>            operations_;
  std::vector<std::string>            ui_thread_operations_;
};

class LoadedLibrary {
 public:
  auto Load(const std::filesystem::path& temp_dir, ApplicationModuleHost& backend,
            const std::vector<LibraryPhotoSpec>& specs, std::size_t count = 0,
            const ProjectHistoryWriter& write_history = {}) -> bool {
    const auto packed = CreatePopulatedPackedProject(temp_dir, {}, count, specs, write_history);
    if (!packed.has_value() || !LoadPackedProject(backend, packed->packed_path_)) {
      return false;
    }
    for (const auto& key : packed->images_) {
      ids_.push_back(key.file_id_);
      image_ids_.push_back(key.image_id_);
    }
    browse_ = backend.project()->handler().project()->GetAlbumBrowseService();
    return browse_ != nullptr && WaitForLibraryQuery(backend);
  }
  auto ids() const -> const std::vector<sl_element_id_t>& { return ids_; }
  auto image_ids() const -> const std::vector<image_id_t>& { return image_ids_; }
  auto browse() const -> const std::shared_ptr<AlbumBrowseService>& { return browse_; }

 private:
  std::vector<sl_element_id_t>        ids_;
  std::vector<image_id_t>             image_ids_;
  std::shared_ptr<AlbumBrowseService> browse_;
};

auto ModelIds(LibraryModule& library) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& item : library.model().items()) {
    ids.push_back(item.element_id);
  }
  return ids;
}

auto Settle(ApplicationModuleHost& backend) -> bool { return WaitForLibraryQuery(backend); }

/// Persist one exposure edit of @p file_id as the head of its active Version, as a Paste to a
/// library image writes it. The commit clock makes it the newest commit of the project.
void PersistExposureEdit(PipelineMgmtService& pipelines, sl_element_id_t file_id) {
  auto           base    = pipelines.LoadHistorySnapshot(file_id);
  const auto     current = pipelines.AcquireCommittedSnapshot(file_id);
  nlohmann::json before =
      current->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure())->ToJson();
  nlohmann::json after = before;
  after["exposure_ev"] = before.at("exposure_ev").get<float>() + 1.0f;

  PipelineEditBatch  batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = std::move(before);
  change.after_value                   = std::move(after);
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));

  CommitGraph graph  = *base.graph_;
  auto        commit = EditCommit::MakePipelineEdit(graph.GetRootId(),
                                                    graph.GetActiveVersionRef().head_commit_hash, batch);
  const auto  head   = commit.GetCommitHash();
  ASSERT_TRUE(graph.InsertCommit(std::move(commit)));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  ASSERT_NE(pipelines.PersistHistory(base, graph), nullptr);
}

/// Photos 4 and then 2 of SixPhotoSpecs are edited, so photo 2 is the last edited photo.
void EditPhotosFourThenTwo(PipelineMgmtService&                           pipelines,
                           const std::vector<PopulatedProject::ImageKey>& images) {
  PersistExposureEdit(pipelines, images[4].file_id_);
  PersistExposureEdit(pipelines, images[2].file_id_);
}

}  // namespace

TEST_F(LibraryQueryTests, SortActionsReplaceDirectionOrClearExplicitSort) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*       module = backend.library();
  const auto& id     = library.ids();

  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("rating"));
  EXPECT_TRUE(module->SortDescending());
  EXPECT_EQ(ModelIds(*module),
            (std::vector<sl_element_id_t>{id[1], id[4], id[0], id[2], id[3], id[5]}));

  // The opposite action on the same field reverses the order.
  module->ToggleInspectorSort(QStringLiteral("rating"), false);
  ASSERT_TRUE(Settle(backend));
  EXPECT_FALSE(module->SortDescending());
  EXPECT_EQ(ModelIds(*module),
            (std::vector<sl_element_id_t>{id[5], id[3], id[0], id[2], id[1], id[4]}));

  // The active action again clears the explicit sort: File ID order, no active arrow.
  module->ToggleInspectorSort(QStringLiteral("rating"), false);
  ASSERT_TRUE(Settle(backend));
  EXPECT_TRUE(module->SortFieldName().isEmpty());
  EXPECT_EQ(ModelIds(*module), id);

  // Another field replaces the previous sort.
  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  module->ToggleInspectorSort(QStringLiteral("import"), false);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("import"));
  EXPECT_EQ(ModelIds(*module),
            (std::vector<sl_element_id_t>{id[4], id[5], id[0], id[1], id[2], id[3]}));

  // An unknown field reports an error and changes nothing.
  module->ToggleInspectorSort(QStringLiteral("aperture"), true);
  EXPECT_FALSE(module->QueryError().isEmpty());
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("import"));
}

TEST_F(LibraryQueryTests, SelectingNewGroupReplacesPreviousGroupWithoutChangingFilters) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();
  auto* stats  = backend.stats();

  stats->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Nikon Z8"));
  module->SetInspectorGrouping(QStringLiteral("date"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("date"));
  EXPECT_EQ(module->section_model().GroupCount(), 1);

  module->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("camera"));
  EXPECT_EQ(stats->StatsFilterCamera(), QStringLiteral("Nikon Z8"));
  EXPECT_EQ(module->TotalCount(), 2);
  ASSERT_EQ(module->section_model().GroupCount(), 1);
  EXPECT_EQ(std::get<std::string>(module->section_model().groups().front().key_), "Nikon Z8");

  // Unchecking another field changes nothing; unchecking the current one returns to flat.
  module->SetInspectorGrouping(QStringLiteral("date"), false);
  EXPECT_FALSE(module->QueryUpdating());
  module->SetInspectorGrouping(QStringLiteral("camera"), false);
  ASSERT_TRUE(Settle(backend));
  EXPECT_FALSE(module->Grouped());
  EXPECT_EQ(module->section_model().GroupCount(), 0);
  EXPECT_EQ(stats->StatsFilterCamera(), QStringLiteral("Nikon Z8"));
  EXPECT_EQ(module->TotalCount(), 2);
}

TEST_F(LibraryQueryTests, GroupingSortedFieldPreservesSortAndFilters) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*       module = backend.library();
  const auto& id     = library.ids();

  backend.stats()->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Canon R5"));
  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  ASSERT_TRUE(Settle(backend));
  module->SetInspectorGrouping(QStringLiteral("rating"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("rating"));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("rating"));
  EXPECT_TRUE(module->SortDescending());
  EXPECT_EQ(backend.stats()->StatsFilterCamera(), QStringLiteral("Canon R5"));
  EXPECT_EQ(ModelIds(*module), (std::vector<sl_element_id_t>{id[1], id[0]}));
}

TEST_F(LibraryQueryTests, PendingSortAndGroupChangesApplyTogether) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module = backend.library();
  QSignalSpy presentation(module, &LibraryModule::PresentationChanged);

  // Three changes in one turn: one accepted result carries all of them.
  module->ToggleInspectorSort(QStringLiteral("import"), true);
  module->SetInspectorGrouping(QStringLiteral("camera"), true);
  backend.stats()->ToggleStatsFilter(QStringLiteral("rating"), QStringLiteral("5"));
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(presentation.count(), 1);
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("import"));
  EXPECT_TRUE(module->SortDescending());
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("camera"));
  EXPECT_EQ(module->TotalCount(), 2);
  EXPECT_EQ(module->section_model().GroupCount(), 2);
}

TEST_F(LibraryQueryTests, PendingGroupChangePreservesSameFieldPendingSort) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module = backend.library();
  WorkerHold hold(library.browse());

  // The sort is on the worker when the group request arrives; both stay requested.
  hold.ArmNext();
  module->ToggleInspectorSort(QStringLiteral("date"), false);
  ASSERT_TRUE(hold.WaitHeld());
  module->SetInspectorGrouping(QStringLiteral("date"), true);
  ProcessEvents(50);
  hold.Release();
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("date"));
  EXPECT_FALSE(module->SortDescending());
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("date"));
}

TEST_F(LibraryQueryTests, EarlierReadCannotReplaceLaterSortSelection) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*                module = backend.library();
  const auto&          id     = library.ids();
  WorkerHold           hold(library.browse());
  std::vector<QString> published_sorts;
  QObject::connect(module, &LibraryModule::PresentationChanged, module,
                   [&]() { published_sorts.push_back(module->SortFieldName()); });

  hold.ArmNext();
  module->ToggleInspectorSort(QStringLiteral("rating"), true);  // read A, held on the worker
  ASSERT_TRUE(hold.WaitHeld());
  module->ToggleInspectorSort(QStringLiteral("camera"), false);  // read B replaces A
  ProcessEvents(50);
  hold.Release();  // A finishes and posts; the UI rejects it
  ASSERT_TRUE(Settle(backend));
  ProcessEvents(200);

  EXPECT_EQ(published_sorts, (std::vector<QString>{QStringLiteral("camera")}));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("camera"));
  EXPECT_EQ(ModelIds(*module),
            (std::vector<sl_element_id_t>{id[0], id[1], id[2], id[3], id[4], id[5]}));
}

TEST_F(LibraryQueryTests, SearchApplyKeepsInspectorFiltersAndPresentationOptions) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();
  auto* stats  = backend.stats();
  auto* search = backend.search();

  stats->ToggleStatsFilter(QStringLiteral("rating"), QStringLiteral("3"));
  module->ToggleInspectorSort(QStringLiteral("import"), true);
  module->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(Settle(backend));
  ASSERT_EQ(module->TotalCount(), 2);

  // album-delete-0 and album-delete-2 have rating 3; the search narrows to album-delete-2.
  search->ApplyFuzzySearch(QStringLiteral("album-delete-2"));
  EXPECT_FALSE(search->HasActiveSearchFilter());  // pending until the refresh is accepted
  ASSERT_TRUE(Settle(backend));
  EXPECT_TRUE(search->HasActiveSearchFilter());
  EXPECT_EQ(stats->StatsFilterRating(), QStringLiteral("3"));
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("import"));
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("camera"));
  EXPECT_EQ(module->TotalCount(), 1);
  EXPECT_EQ(ModelIds(*module), (std::vector<sl_element_id_t>{library.ids()[2]}));

  // Clearing the search removes only the search term.
  search->ClearFuzzySearch();
  ASSERT_TRUE(Settle(backend));
  EXPECT_FALSE(search->HasActiveSearchFilter());
  EXPECT_EQ(stats->StatsFilterRating(), QStringLiteral("3"));
  EXPECT_EQ(module->TotalCount(), 2);
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("camera"));
}

TEST_F(LibraryQueryTests, GroupingPreservesSearchAndInspectorPredicates) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();

  backend.stats()->ToggleStatsFilter(QStringLiteral("rating"), QStringLiteral("5"));
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  ASSERT_TRUE(Settle(backend));
  const auto flat = ModelIds(*module);
  ASSERT_EQ(flat.size(), 2u);
  for (const auto* field : {"date", "import", "camera", "lens", "rating", "label"}) {
    module->SetInspectorGrouping(QString::fromLatin1(field), true);
    ASSERT_TRUE(Settle(backend));
    auto grouped = ModelIds(*module);
    EXPECT_EQ(std::set<sl_element_id_t>(grouped.begin(), grouped.end()),
              std::set<sl_element_id_t>(flat.begin(), flat.end()))
        << field;
    EXPECT_EQ(module->section_model().UniqueFileCount(), 2) << field;
  }
}

TEST_F(LibraryQueryTests, ReopenDisplaysPersistedImportDate) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  const auto& model = backend.library()->model();
  const auto  specs = SixPhotoSpecs();
  ASSERT_EQ(model.items().size(), specs.size());
  for (size_t index = 0; index < specs.size(); ++index) {
    const auto expected = QDateTime::fromSecsSinceEpoch(*specs[index].added_time_, QTimeZone::utc())
                              .toLocalTime()
                              .date();
    EXPECT_EQ(model.items()[index].import_date, expected) << index;
    EXPECT_NE(model.items()[index].import_date, QDate::currentDate()) << index;
    EXPECT_EQ(model.data(model.index(static_cast<int>(index)), AlbumThumbnailModel::ImportDate)
                  .toString(),
              expected.toString(QStringLiteral("yyyy-MM-dd")));
  }
}

TEST_F(LibraryQueryTests, EqualSortKeysKeepDeterministicFocusPosition) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();
  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  module->SetInspectorGrouping(QStringLiteral("date"), true);
  ASSERT_TRUE(Settle(backend));

  QSignalSpy positions(module, &LibraryModule::FocusPositionReady);
  const auto order = ModelIds(*module);
  for (size_t index = 0; index < order.size(); ++index) {
    module->RequestFocusPosition(static_cast<uint>(order[index]));
    ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }));
    const auto args = positions.takeFirst();
    EXPECT_EQ(args.at(0).toUInt(), static_cast<uint>(order[index]));
    EXPECT_EQ(args.at(1).toLongLong(), static_cast<qint64>(index));
    EXPECT_EQ(args.at(2).toInt(),
              module->section_model().RowForOccurrence(static_cast<qint64>(index)));
  }
  // A file outside the filtered result reports no position.
  backend.stats()->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Canon R5"));
  module->RequestFocusPosition(static_cast<uint>(library.ids()[4]));
  ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }));
  EXPECT_EQ(positions.takeFirst().at(1).toLongLong(), -1);
}

// Opening a project makes its last edited photo the pending reveal and the editor's last image;
// the reveal reads the photo's position in the accepted order once and then ends.
TEST_F(LibraryQueryTests, ProjectOpenRevealsTheLastEditedPhoto) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs(), 0, EditPhotosFourThenTwo));
  auto*      module = backend.library();
  const auto target = static_cast<uint>(library.ids()[2]);
  ASSERT_TRUE(WaitUntil([&]() { return module->PendingRevealFileId() == target; }));
  EXPECT_EQ(backend.editor_session()->last_element_id(), target);
  EXPECT_EQ(backend.editor_session()->last_image_id(), static_cast<uint>(library.image_ids()[2]));

  QSignalSpy positions(module, &LibraryModule::FocusPositionReady);
  module->RequestPendingReveal();
  ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }));
  const auto args = positions.takeFirst();
  EXPECT_EQ(args.at(0).toUInt(), target);
  EXPECT_EQ(args.at(1).toLongLong(), 2);  // the default order is by file id
  EXPECT_EQ(module->PendingRevealFileId(), 0u);
  module->RequestPendingReveal();
  ProcessEvents(100);
  EXPECT_EQ(positions.count(), 0);

  // The edit-time sort and edit-day groups read the same edits.
  module->ToggleInspectorSort(QStringLiteral("edited"), true);
  ASSERT_TRUE(Settle(backend));
  const auto sorted = ModelIds(*module);
  ASSERT_EQ(sorted.size(), 6u);
  EXPECT_EQ(sorted[0], library.ids()[2]);
  EXPECT_EQ(sorted[1], library.ids()[4]);
  module->SetInspectorGrouping(QStringLiteral("edited"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(module->GroupFieldName(), QStringLiteral("edited"));
  EXPECT_EQ(module->section_model().GroupCount(), 2);  // today's edit day and the unedited group
}

// A pending reveal outside the current folder selects the root folder once and finds the photo
// there; a project without edits has no pending reveal.
TEST_F(LibraryQueryTests, RevealSelectsTheRootFolderWhenTheCurrentFolderLacksThePhoto) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs(), 0, EditPhotosFourThenTwo));
  auto*      module  = backend.library();
  auto*      folders = backend.folders();
  const auto target  = static_cast<uint>(library.ids()[2]);
  ASSERT_TRUE(WaitUntil([&]() { return module->PendingRevealFileId() == target; }));

  folders->CreateFolder(QStringLiteral("Empty"));
  uint empty_folder = 0;
  for (const auto& entry : folders->Folders()) {
    const auto map = entry.toMap();
    if (map.value(QStringLiteral("folderId")).toUInt() != 0) {
      empty_folder = map.value(QStringLiteral("folderId")).toUInt();
    }
  }
  ASSERT_NE(empty_folder, 0u);
  folders->SelectFolder(empty_folder);
  ASSERT_TRUE(Settle(backend));
  ASSERT_EQ(folders->CurrentFolderId(), empty_folder);
  EXPECT_TRUE(ModelIds(*module).empty());

  QSignalSpy positions(module, &LibraryModule::FocusPositionReady);
  module->RequestPendingReveal();
  ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }));
  const auto args = positions.takeFirst();
  EXPECT_EQ(args.at(0).toUInt(), target);
  EXPECT_EQ(args.at(1).toLongLong(), 2);
  EXPECT_EQ(folders->CurrentFolderId(), 0u);
  EXPECT_EQ(module->PendingRevealFileId(), 0u);
  EXPECT_EQ(ModelIds(*module).size(), 6u);
}

TEST_F(LibraryQueryTests, ProjectWithoutEditsHasNoPendingReveal) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  ProcessEvents(200);
  EXPECT_EQ(backend.library()->PendingRevealFileId(), 0u);
  EXPECT_EQ(backend.editor_session()->last_element_id(), 0u);
}

// An edit written while the library is hidden (the editor is open) does not reorder the photos
// under the user; the edit-ordered library refreshes when it is shown again. While the library
// is shown, an edit refreshes it at once.
TEST_F(LibraryQueryTests, EditOrderRefreshWaitsUntilTheLibraryIsShown) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs(), 0, EditPhotosFourThenTwo));
  auto* module = backend.library();
  module->ToggleInspectorSort(QStringLiteral("edited"), true);
  ASSERT_TRUE(Settle(backend));
  const auto before = ModelIds(*module);
  ASSERT_EQ(before.front(), library.ids()[2]);
  auto& pipelines = *backend.project()->handler().pipeline_service();

  PersistExposureEdit(pipelines, library.ids()[0]);
  module->NoteEditHistoryChanged(true);
  ProcessEvents(100);
  EXPECT_FALSE(module->QueryUpdating());
  EXPECT_EQ(ModelIds(*module), before);

  module->RefreshStaleEditOrder();
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(ModelIds(*module).front(), library.ids()[0]);
  // The refresh applied the stale edit; showing the library again reads nothing.
  module->RefreshStaleEditOrder();
  EXPECT_FALSE(module->QueryUpdating());

  PersistExposureEdit(pipelines, library.ids()[5]);
  module->NoteEditHistoryChanged(false);
  ASSERT_TRUE(Settle(backend));
  EXPECT_EQ(ModelIds(*module).front(), library.ids()[5]);
}

TEST_F(LibraryQueryTests, OrderedFileIdsAreUniqueAndFollowTheAcceptedOrder) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();
  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  module->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(Settle(backend));

  QSignalSpy ids(module, &LibraryModule::OrderedFileIdsReady);
  const auto request = module->RequestAllFileIds();
  ASSERT_TRUE(WaitUntil([&]() { return ids.count() == 1; }));
  const auto args = ids.takeFirst();
  EXPECT_EQ(args.at(0).toULongLong(), request);
  std::vector<sl_element_id_t> all;
  for (const auto& value : args.at(1).toList()) {
    all.push_back(value.toMap().value("elementId").toUInt());
  }
  EXPECT_EQ(all, ModelIds(*module));

  // Shift selection over a collapsed group skips its photos.
  module->section_model().SetGroupCollapsed(1, true);  // Nikon Z8
  module->RequestOrderedFileIds(module->section_model().VisibleOccurrenceRanges(0, 5));
  ASSERT_TRUE(WaitUntil([&]() { return ids.count() == 1; }));
  std::vector<sl_element_id_t> visible;
  for (const auto& value : ids.takeFirst().at(1).toList()) {
    visible.push_back(value.toMap().value("elementId").toUInt());
  }
  const auto& id = library.ids();
  EXPECT_EQ(visible, (std::vector<sl_element_id_t>{id[1], id[0], id[4], id[5]}));
}

TEST_F(LibraryQueryTests, LastVisibleOccurrenceReleasesThumbnailPin) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module   = backend.library();
  const auto file_id  = static_cast<uint>(library.ids()[0]);
  const auto image_id = static_cast<uint>(module->FindAlbumItem(file_id)->image_id);

  module->SetInspectorGrouping(QStringLiteral("label"), true);
  ASSERT_TRUE(Settle(backend));
  // Two visible occurrences of one photo (two label groups) share one pin.
  module->SetOccurrenceThumbnailVisible(QStringLiteral("portrait"), false, file_id, image_id, true,
                                        256);
  module->SetOccurrenceThumbnailVisible(QStringLiteral("landscape"), false, file_id, image_id, true,
                                        256);
  EXPECT_TRUE(module->thumbs().IsThumbnailPinned(file_id));
  EXPECT_EQ(module->thumbs().VisibleOccurrenceCount(file_id), 2u);
  module->SetOccurrenceThumbnailVisible(QStringLiteral("portrait"), false, file_id, image_id, false,
                                        256);
  EXPECT_TRUE(module->thumbs().IsThumbnailPinned(file_id));
  module->SetOccurrenceThumbnailVisible(QStringLiteral("landscape"), false, file_id, image_id,
                                        false, 256);
  EXPECT_FALSE(module->thumbs().IsThumbnailPinned(file_id));
  EXPECT_EQ(module->thumbs().VisibleOccurrenceCount(file_id), 0u);
}

TEST_F(LibraryQueryTests, RepeatedOccurrenceVisibilityIsIdempotent) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module   = backend.library();
  const auto file_id  = static_cast<uint>(library.ids()[1]);
  const auto image_id = static_cast<uint>(module->FindAlbumItem(file_id)->image_id);

  for (int repeat = 0; repeat < 3; ++repeat) {
    module->SetOccurrenceThumbnailVisible(QString(), true, file_id, image_id, true, 256);
  }
  module->SetOccurrenceThumbnailVisible(QStringLiteral("other"), false, file_id, image_id, true,
                                        256);
  EXPECT_EQ(module->thumbs().VisibleOccurrenceCount(file_id), 2u);
  // One release per occurrence: repeated true did not add a pin that leaks.
  module->SetOccurrenceThumbnailVisible(QString(), true, file_id, image_id, false, 256);
  module->SetOccurrenceThumbnailVisible(QString(), true, file_id, image_id, false, 256);
  EXPECT_TRUE(module->thumbs().IsThumbnailPinned(file_id));
  module->SetOccurrenceThumbnailVisible(QStringLiteral("other"), false, file_id, image_id, false,
                                        256);
  EXPECT_FALSE(module->thumbs().IsThumbnailPinned(file_id));
}

TEST_F(LibraryQueryTests, LabelGroupOccurrencesShareThePhotoRowAndRatingUpdates) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto* module = backend.library();
  module->SetInspectorGrouping(QStringLiteral("label"), true);
  ASSERT_TRUE(Settle(backend));
  // Without a semantic model every photo is in the unlabelled group, once.
  ASSERT_EQ(module->section_model().GroupCount(), 1);
  EXPECT_EQ(module->section_model().OccurrenceTotal(), 6);
  EXPECT_EQ(module->model().count(), 6);

  // A rating change goes through its owner and the full refresh shows it on the photo row.
  const auto file_id  = static_cast<uint>(library.ids()[3]);
  const auto image_id = static_cast<uint>(module->FindAlbumItem(file_id)->image_id);
  backend.images()->SetImageRating(file_id, image_id, 4);
  ASSERT_TRUE(Settle(backend));
  const auto row = module->model().rowByElementId(file_id);
  ASSERT_GE(row, 0);
  EXPECT_EQ(module->model().items()[static_cast<size_t>(row)].rating, 4);
}

TEST_F(LibraryQueryTests, AlbumQuerySqlRunsOnWorkerThread) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module = backend.library();
  WorkerHold observer(library.browse());

  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  ASSERT_TRUE(Settle(backend));
  module->SetInspectorGrouping(QStringLiteral("import"), true);
  ASSERT_TRUE(Settle(backend));
  backend.stats()->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Sony A7"));
  ASSERT_TRUE(Settle(backend));
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  ASSERT_TRUE(Settle(backend));
  QSignalSpy positions(module, &LibraryModule::FocusPositionReady);
  module->RequestFocusPosition(static_cast<uint>(library.ids()[4]));
  ASSERT_TRUE(WaitUntil([&]() { return positions.count() == 1; }));
  QSignalSpy ids(module, &LibraryModule::OrderedFileIdsReady);
  module->RequestAllFileIds();
  ASSERT_TRUE(WaitUntil([&]() { return ids.count() == 1; }));

  EXPECT_GE(observer.Operations().size(), 6u);
  EXPECT_TRUE(observer.UiThreadOperations().empty());
}

TEST_F(LibraryQueryTests, QueryFailureKeepsAcceptedContentAndRetryRepeatsTheRequest) {
  ApplicationModuleHost backend;
  LoadedLibrary         library;
  ASSERT_TRUE(library.Load(temp_dir_, backend, SixPhotoSpecs()));
  auto*      module = backend.library();
  const auto before = ModelIds(*module);

  // Remove the Image table under the query so the real read fails on the worker.
  {
    auto guard =
        backend.project()->handler().project()->GetStorage()->GetDatabase().GetConnectionGuard();
    auto          lock = guard.Lock();
    duckdb_result result;
    ASSERT_EQ(duckdb_query(guard.conn_, "ALTER TABLE Image RENAME TO ImageHidden", &result),
              DuckDBSuccess);
    duckdb_destroy_result(&result);
  }
  module->ToggleInspectorSort(QStringLiteral("rating"), true);
  ASSERT_TRUE(Settle(backend));
  EXPECT_FALSE(module->QueryError().isEmpty());
  EXPECT_TRUE(module->SortFieldName().isEmpty());  // the accepted options stay
  EXPECT_EQ(ModelIds(*module), before);            // the accepted content stays

  {
    auto guard =
        backend.project()->handler().project()->GetStorage()->GetDatabase().GetConnectionGuard();
    auto          lock = guard.Lock();
    duckdb_result result;
    ASSERT_EQ(duckdb_query(guard.conn_, "ALTER TABLE ImageHidden RENAME TO Image", &result),
              DuckDBSuccess);
    duckdb_destroy_result(&result);
  }
  module->RetryLibraryQuery();
  ASSERT_TRUE(Settle(backend));
  EXPECT_TRUE(module->QueryError().isEmpty());
  EXPECT_EQ(module->SortFieldName(), QStringLiteral("rating"));
  EXPECT_TRUE(module->SortDescending());
}

// Pages: 130 photos and a search, so a page holds 120 rows.
TEST_F(LibraryQueryTests, QueuedPageCannotAppendAfterFilterRefresh) {
  ApplicationModuleHost        backend;
  LoadedLibrary                library;
  std::vector<LibraryPhotoSpec> specs(130);
  for (size_t index = 0; index < specs.size(); ++index) {
    specs[index].rating_ = index % 2 == 0 ? 5 : 1;
  }
  ASSERT_TRUE(library.Load(temp_dir_, backend, specs));
  auto* module = backend.library();
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  ASSERT_TRUE(Settle(backend));
  ASSERT_EQ(module->ShownCount(), 120);
  ASSERT_TRUE(module->HasMoreThumbnails());

  WorkerHold hold(library.browse());
  hold.ArmNext();
  ASSERT_TRUE(module->LoadMoreThumbnails());  // page read, held on the worker
  ASSERT_TRUE(hold.WaitHeld());
  backend.stats()->ToggleStatsFilter(QStringLiteral("rating"), QStringLiteral("1"));
  ProcessEvents(50);  // the refresh is submitted while the page is held
  hold.Release();
  ASSERT_TRUE(Settle(backend));
  ProcessEvents(200);

  EXPECT_EQ(module->TotalCount(), 65);
  EXPECT_EQ(module->ShownCount(), 65);
  for (const auto& item : module->model().items()) {
    EXPECT_EQ(item.rating, 1);
  }
}

TEST_F(LibraryQueryTests, DistantSectionReadKeepsMetadataPagesBounded) {
  ApplicationModuleHost        backend;
  LoadedLibrary                library;
  std::vector<LibraryPhotoSpec> specs(600);
  for (size_t index = 0; index < specs.size(); ++index) {
    specs[index].model_ = index < 500 ? "Canon R5" : "Camera " + std::to_string(index % 20);
  }
  ASSERT_TRUE(library.Load(temp_dir_, backend, specs));
  auto* module = backend.library();
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  module->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(Settle(backend));
  auto& sections = module->section_model();
  sections.SetColumnCount(6);
  ASSERT_EQ(sections.OccurrenceTotal(), 600);

  size_t max_pages = 0;
  int    max_rows  = 0;
  for (const qint64 occurrence : {590, 250, 480, 10, 599, 300}) {
    const int row = sections.RowForOccurrence(occurrence);
    module->RequestSectionRows(row, row + 2);
    ASSERT_TRUE(WaitUntil([&]() { return sections.FileIdAt(occurrence) != 0; })) << occurrence;
    max_pages = std::max(max_pages, sections.LoadedPageCount());
    max_rows  = std::max(max_rows, module->model().count());
    RecordProperty("loaded_pages_after_" + std::to_string(occurrence),
                   std::to_string(sections.LoadedPageCount()));
  }
  // Pages of 120 occurrences, at most 3 kept: never all 600 photos.
  EXPECT_LE(max_pages, 3u);
  EXPECT_LE(max_rows, 360);
  std::cout << "[library query] max loaded pages " << max_pages << ", max photo rows " << max_rows
            << "\n";
}

}  // namespace alcedo::ui::test
