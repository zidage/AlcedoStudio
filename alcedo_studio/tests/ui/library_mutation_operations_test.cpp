//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LibraryMutationOperations over a real project with CI RAW files. The editor session of the
// delete tests is an EditorSessionController over a recording backend, so the test can hold the
// deleted image open without a GPU.

#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using alcedo::EditorSessionIdentity;
using alcedo::EditorSessionResult;
using alcedo::EditorSessionResultKind;
using alcedo::EditorSessionState;

/// Editor session backend whose Open and Close finish at once.
class ImmediateSessionBackend final : public alcedo::IEditorSessionBackend {
 public:
  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  [[nodiscard]] auto has_image() const -> bool override {
    return identity_.element_id > 0 && alcedo::EditorSessionHasImage(state_);
  }
  [[nodiscard]] auto last_error() const -> std::string override { return {}; }
  void               SetPresentationSinkId(alcedo::PresentationSinkId) override {}
  void               SetPresentationSize(int, int) override {}
  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    state_ = element_id == 0 ? EditorSessionState::NoImage : EditorSessionState::Interactive;
    NotifyChange();
    return Result();
  }
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    return Open(element_id, image_id);
  }
  auto Close(bool persist_changes) -> EditorSessionResult override {
    ++close_count_;
    last_close_persist_ = persist_changes;
    state_              = EditorSessionState::NoImage;
    identity_           = {};
    NotifyChange();
    return Result();
  }
  auto Shutdown() -> EditorSessionResult override { return Close(true); }
  auto Discard() -> EditorSessionResult override { return Result(); }
  auto Undo() -> EditorSessionResult override { return Result(); }
  auto Redo() -> EditorSessionResult override { return Result(); }

  int  close_count_        = 0;
  bool last_close_persist_ = true;

 private:
  auto Result() const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::StateChanged;
    result.state    = state_;
    result.identity = identity_;
    return result;
  }

  EditorSessionState    state_ = EditorSessionState::NoImage;
  EditorSessionIdentity identity_{};
};

auto CollectCiRawFiles(size_t max_count) -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

class LibraryMutationOperationsTest : public ApplicationModuleHostTestFixture {
 protected:
  /// Creates a project, imports @p count CI RAW files, and returns their library rows.
  auto ImportCiRawFiles(ApplicationModuleHost& host, size_t count) -> QVariantList {
    const auto files = CollectCiRawFiles(count);
    EXPECT_EQ(files.size(), count) << "the CI RAW files are missing under " << TEST_IMG_PATH;
    EXPECT_TRUE(CreateTestProject(host));
    host.import_export()->StartImport(PathsToQStringList(files));
    EXPECT_TRUE(WaitUntil([&] { return host.IsIdle(); }, std::chrono::minutes(2)));
    EXPECT_TRUE(WaitUntil(
        [&] { return host.library()->Thumbnails().size() == static_cast<qsizetype>(count); },
        std::chrono::seconds(30)));
    return host.library()->Thumbnails();
  }
};

TEST_F(LibraryMutationOperationsTest, DeleteOpenEditorImageClosesEditorImage) {
  ApplicationModuleHost host;
  const QVariantList    rows = ImportCiRawFiles(host, 1);
  ASSERT_EQ(rows.size(), 1);
  const QVariantMap       row      = rows.front().toMap();
  const uint              element  = row.value(QStringLiteral("elementId")).toUInt();
  const uint              image_id = row.value(QStringLiteral("imageId")).toUInt();

  ImmediateSessionBackend backend;
  EditorSessionController editor(&backend);
  WorkspaceRouter         router(&editor);
  editor.Open(element, image_id);
  ASSERT_EQ(editor.session_state(), EditorSessionState::Interactive);
  host.library()->selection()->SetImageSelected(element, image_id, QString(), false, true);

  LibraryMutationOperations operations(host.images(), host.library()->selection(), host.folders(),
                                       &editor, &router);
  EXPECT_EQ(operations.DeleteScope(), QStringLiteral("project"));
  const QVariantList targets = operations.ResolveTargets(QVariantMap{}, true);
  ASSERT_EQ(targets.size(), 1);
  const QVariantMap result = operations.DeleteTargets(targets);

  EXPECT_EQ(result.value(QStringLiteral("deletedElementIds")).toList(),
            (QVariantList{QVariant(element)}));
  EXPECT_EQ(editor.session_state(), EditorSessionState::NoImage);
  EXPECT_EQ(backend.close_count_, 1);
  EXPECT_FALSE(backend.last_close_persist_);
  EXPECT_EQ(editor.last_element_id(), 0u);
  EXPECT_EQ(host.library()->selection()->SelectedCount(), 0);
}

TEST_F(LibraryMutationOperationsTest, DeleteOpenImageInEditorWorkspaceShowsEmptyEditor) {
  ApplicationModuleHost host;
  const QVariantList    rows = ImportCiRawFiles(host, 1);
  ASSERT_EQ(rows.size(), 1);
  const QVariantMap       row      = rows.front().toMap();
  const uint              element  = row.value(QStringLiteral("elementId")).toUInt();
  const uint              image_id = row.value(QStringLiteral("imageId")).toUInt();

  ImmediateSessionBackend backend;
  EditorSessionController editor(&backend);
  WorkspaceRouter         router(&editor);
  router.OpenEditor(element, image_id);
  ASSERT_EQ(editor.element_id(), element);

  LibraryMutationOperations operations(host.images(), host.library()->selection(), host.folders(),
                                       &editor, &router);
  const QVariantMap         result = operations.DeleteTargets(operations.ResolveTargets(row, true));

  EXPECT_EQ(result.value(QStringLiteral("deletedCount")).toInt(), 1);
  EXPECT_EQ(router.workspace(), QStringLiteral("editor"));
  EXPECT_EQ(router.element_id(), 0u);
  EXPECT_FALSE(editor.has_image());
}

TEST_F(LibraryMutationOperationsTest, BatchRatingUsesBatchPath) {
  ApplicationModuleHost host;
  const QVariantList    rows = ImportCiRawFiles(host, 2);
  ASSERT_EQ(rows.size(), 2);
  LibraryMutationOperations* operations = host.library_mutations();

  QSignalSpy                 finished(host.images(), &ImageController::ImageRatingsFinished);
  const QVariantMap          result = operations->RateTargets(rows, 4);
  EXPECT_TRUE(result.value(QStringLiteral("batch")).toBool());
  EXPECT_TRUE(result.value(QStringLiteral("started")).toBool());
  const QString task_id = result.value(QStringLiteral("taskId")).toString();
  EXPECT_FALSE(task_id.isEmpty());
  int rating_tasks = 0;
  for (const QVariant& task : host.background_tasks()->Tasks()) {
    if (task.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("ratingUpdate")) {
      ++rating_tasks;
    }
  }
  EXPECT_EQ(rating_tasks, 1);
  ASSERT_TRUE(WaitForSignal(finished, 60000));
  EXPECT_TRUE(finished.front().front().toMap().value(QStringLiteral("success")).toBool());
}

TEST_F(LibraryMutationOperationsTest, SingleRatingUsesDirectPathAndRejectsOutOfRange) {
  ApplicationModuleHost host;
  const QVariantList    rows = ImportCiRawFiles(host, 1);
  ASSERT_EQ(rows.size(), 1);
  LibraryMutationOperations* operations = host.library_mutations();

  EXPECT_FALSE(operations->RateTargets(rows, 6).value(QStringLiteral("success")).toBool());
  EXPECT_FALSE(operations->RateTargets(rows, -1).value(QStringLiteral("success")).toBool());
  const QVariantMap result = operations->RateTargets(rows, 2);
  EXPECT_FALSE(result.value(QStringLiteral("batch")).toBool());
  EXPECT_TRUE(result.value(QStringLiteral("success")).toBool())
      << result.value(QStringLiteral("message")).toString().toStdString();
  EXPECT_EQ(result.value(QStringLiteral("rating")).toInt(), 2);
}

TEST_F(LibraryMutationOperationsTest, ResolveTargetsUsesSelectionOnlyWhenAsked) {
  ApplicationModuleHost      host;
  LibraryMutationOperations* operations = host.library_mutations();
  const QVariantMap clicked{{QStringLiteral("elementId"), 9u}, {QStringLiteral("imageId"), 90u}};
  host.library()->selection()->SetImageSelected(4, 40, QStringLiteral("d.arw"), false, true);

  const QVariantList with_selection = operations->ResolveTargets(clicked, true);
  ASSERT_EQ(with_selection.size(), 1);
  EXPECT_EQ(with_selection.front().toMap().value(QStringLiteral("elementId")).toUInt(), 4u);
  const QVariantList clicked_only = operations->ResolveTargets(clicked, false);
  ASSERT_EQ(clicked_only.size(), 1);
  EXPECT_EQ(clicked_only.front().toMap().value(QStringLiteral("elementId")).toUInt(), 9u);
  EXPECT_EQ(clicked_only.front().toMap().value(QStringLiteral("fileId")).toUInt(), 9u);
  EXPECT_TRUE(operations->ResolveTargets(QVariantMap{}, false).isEmpty());
}

}  // namespace
}  // namespace alcedo::ui::test
