//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_mini_git_project_fixture.hpp
/// @brief Real ProjectService + dual-image Mini-Git environment for persistence tests.
///
/// Owns one temporary project directory, two persisted default Versions with distinct
/// element/root IDs, deterministic CommitClockAccess timestamps, and helpers that
/// append edits, capture working state, reopen the project, and inspect stored graph
/// and journal records. Does not construct session lifecycle, QML, or render ports.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo::test {

/// GoogleTest-friendly project fixture for Mini-Git materialization and recovery.
///
/// Lifetime: SetUp creates the temp project and two empty persisted graphs; TearDown
/// destroys materializer/storage/project objects before deleting files.
class EditorMiniGitProjectFixture {
 public:
  static constexpr sl_element_id_t kElementA = 101;
  static constexpr sl_element_id_t kElementB = 202;

  EditorMiniGitProjectFixture()  = default;
  ~EditorMiniGitProjectFixture() = default;

  EditorMiniGitProjectFixture(const EditorMiniGitProjectFixture&)            = delete;
  EditorMiniGitProjectFixture& operator=(const EditorMiniGitProjectFixture&) = delete;

  /// Create the temporary project directory, open ProjectService, and persist
  /// default Versions for images A and B with distinct root IDs.
  ///
  /// Side effects: creates files under a unique temp directory and resets the
  /// global commit clock to a deterministic baseline.
  void SetUp();

  /// Destroy materializer, storage, and project, then remove temp files.
  ///
  /// Preconditions: safe to call even when SetUp partially failed.
  void TearDown();

  /// Append one exposure edit to the working history for the given element.
  ///
  /// @param element_id  Must be kElementA or kElementB.
  /// @param before      Exposure value before the edit.
  /// @param after       Exposure value after the edit.
  /// @param error       Optional failure detail.
  /// @return true when the journal append and working-head advance both succeed.
  auto AppendExposureEdit(sl_element_id_t element_id, float before, float after,
                          std::string* error = nullptr) -> bool;

  /// Capture the immutable working state used by a save checkpoint for one image.
  ///
  /// @param element_id  Must be kElementA or kElementB.
  /// @param exposure    Serialized pipeline exposure value stored with the capture.
  /// @return Populated capture; journal_records empty when no edits were appended.
  auto CaptureWorkingState(sl_element_id_t element_id, float exposure) -> EditorMiniGitSaveCapture;

  /// Close ProjectService and reopen it on the same database/metadata paths.
  ///
  /// Side effects: drops live graph pointers and reloads Storage. Working
  /// histories and journals are recreated against the reopened storage.
  void CloseAndReopenProject();

  /// Load the stored CommitGraph for one element from DuckDB.
  ///
  /// @return nullopt when the element has no persisted image edit state.
  auto LoadStoredGraph(sl_element_id_t element_id) -> std::optional<CommitGraph>;

  /// Read durable journal records for one element from its journal path.
  auto ReadJournalRecords(sl_element_id_t element_id, std::string* error = nullptr)
      -> std::vector<MiniGitJournalRecord>;

  /// Count commit rows stored for the root of the given element.
  auto CountStoredCommits(sl_element_id_t element_id) -> std::uint64_t;

  /// Run Materialize while holding the project-owned save lock for the capture
  /// element. Matches the production precondition that EditorSaveCheckpointService
  /// holds the lock across materialization.
  auto MaterializeUnderSaveLock(const EditorMiniGitSaveCapture& capture,
                                std::string* error = nullptr) -> EditorMiniGitMaterializeResult;

  [[nodiscard]] auto project() -> ProjectService& { return *project_; }
  [[nodiscard]] auto storage() -> const std::shared_ptr<Storage>& { return storage_; }
  [[nodiscard]] auto materializer() -> EditorMiniGitMaterializer& { return *materializer_; }
  [[nodiscard]] auto save_coordinator() -> EditorSaveCheckpointCoordinator& {
    return *save_coordinator_;
  }
  [[nodiscard]] auto root_dir() const -> const std::filesystem::path& { return root_dir_; }
  [[nodiscard]] auto db_path() const -> const std::filesystem::path& { return db_path_; }
  [[nodiscard]] auto meta_path() const -> const std::filesystem::path& { return meta_path_; }
  [[nodiscard]] auto journal_path(sl_element_id_t element_id) const -> std::filesystem::path;
  [[nodiscard]] auto graph(sl_element_id_t element_id) -> std::shared_ptr<CommitGraph>;
  [[nodiscard]] auto working_history(sl_element_id_t element_id) -> MiniGitWorkingHistory&;
  [[nodiscard]] auto journal(sl_element_id_t element_id) -> MiniGitJournal&;
  [[nodiscard]] auto root_id(sl_element_id_t element_id) const -> root_id_t;

 private:
  struct ImageRuntime {
    sl_element_id_t                     element_id = 0;
    root_id_t                           root_id{};
    std::filesystem::path               journal_path;
    std::shared_ptr<CommitGraph>        graph;
    std::shared_ptr<MiniGitJournal>     journal;
    std::unique_ptr<MiniGitWorkingHistory> history;
  };

  auto RuntimeFor(sl_element_id_t element_id) -> ImageRuntime&;
  auto RuntimeFor(sl_element_id_t element_id) const -> const ImageRuntime&;
  void OpenProjectObjects();
  void CloseProjectObjects();
  void CreatePersistedImage(ImageRuntime& runtime, sl_element_id_t element_id,
                            const std::string& version_name);
  void RebuildWorkingRuntimes();

  std::filesystem::path                      root_dir_;
  std::filesystem::path                      db_path_;
  std::filesystem::path                      meta_path_;
  std::unique_ptr<ProjectService>                      project_;
  std::shared_ptr<Storage>                      storage_;
  std::shared_ptr<EditorSaveCheckpointCoordinator>     save_coordinator_;
  std::unique_ptr<EditorMiniGitMaterializer>           materializer_;
  ImageRuntime                                         image_a_;
  ImageRuntime                                         image_b_;
};

}  // namespace alcedo::test
