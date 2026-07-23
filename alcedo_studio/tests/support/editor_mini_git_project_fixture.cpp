//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "support/editor_mini_git_project_fixture.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "edit/history/commit_clock_test_access.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::test {
namespace {

auto MakeExposurePayload(float before, float after) -> OrdinaryEditPayload {
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "$operator_params";
  payload.before_value   = nlohmann::json{{"exposure", before}};
  payload.after_value    = nlohmann::json{{"exposure", after}};
  payload.before_enabled = true;
  payload.after_enabled  = true;
  return payload;
}

}  // namespace

void EditorMiniGitProjectFixture::SetUp() {
  TimeProvider::Refresh();
  RegisterAllOperators();
  edit_history_test::CommitClockAccess::ResetGlobal(1'000'000'000ULL);

  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  root_dir_  = std::filesystem::temp_directory_path() / ("alcedo_mini_git_proj_" + stamp);
  db_path_   = root_dir_ / "project.db";
  meta_path_ = root_dir_ / "project.json";
  std::filesystem::create_directories(root_dir_);

  OpenProjectObjects();
  CreatePersistedImage(image_a_, kElementA, "DefaultA");
  CreatePersistedImage(image_b_, kElementB, "DefaultB");
  RebuildWorkingRuntimes();
}

void EditorMiniGitProjectFixture::TearDown() {
  CloseProjectObjects();
  std::error_code ec;
  std::filesystem::remove_all(root_dir_, ec);
}

auto EditorMiniGitProjectFixture::journal_path(sl_element_id_t element_id) const
    -> std::filesystem::path {
  return RuntimeFor(element_id).journal_path;
}

auto EditorMiniGitProjectFixture::graph(sl_element_id_t element_id) -> std::shared_ptr<CommitGraph> {
  return RuntimeFor(element_id).graph;
}

auto EditorMiniGitProjectFixture::working_history(sl_element_id_t element_id)
    -> MiniGitWorkingHistory& {
  return *RuntimeFor(element_id).history;
}

auto EditorMiniGitProjectFixture::journal(sl_element_id_t element_id) -> MiniGitJournal& {
  return *RuntimeFor(element_id).journal;
}

auto EditorMiniGitProjectFixture::root_id(sl_element_id_t element_id) const -> root_id_t {
  return RuntimeFor(element_id).root_id;
}

auto EditorMiniGitProjectFixture::AppendExposureEdit(sl_element_id_t element_id, float before,
                                                     float after, std::string* error) -> bool {
  auto& runtime = RuntimeFor(element_id);
  auto  result  = runtime.history->AppendEdit(MakeExposurePayload(before, after));
  if (!result.committed) {
    if (error != nullptr) {
      *error = result.error.empty() ? "AppendExposureEdit failed" : result.error;
    }
    return false;
  }
  return true;
}

auto EditorMiniGitProjectFixture::CaptureWorkingState(sl_element_id_t element_id, float exposure)
    -> EditorMiniGitSaveCapture {
  auto&                    runtime = RuntimeFor(element_id);
  EditorMiniGitSaveCapture capture;
  capture.element_id             = element_id;
  capture.working_head           = runtime.history->working_head();
  capture.transaction_chain_hash = runtime.history->transaction_chain_hash();
  capture.journal_records        = runtime.journal->records();
  capture.journal_path           = runtime.journal_path;
  capture.journal_already_materialized = capture.journal_records.empty();
  const auto serialized          = MakeEditorSerializedPipelineState(
      runtime.graph->GetRootId(), capture.working_head, capture.transaction_chain_hash,
      nlohmann::json{{"exposure", exposure}});
  capture.materialization =
      runtime.graph->CaptureMaterializationWithSerializedPipelineState(serialized);
  return capture;
}

void EditorMiniGitProjectFixture::CloseAndReopenProject() {
  CloseProjectObjects();
  OpenProjectObjects();
  RebuildWorkingRuntimes();
}

auto EditorMiniGitProjectFixture::LoadStoredGraph(sl_element_id_t element_id)
    -> std::optional<CommitGraph> {
  auto guard = storage_->GetDBController().GetConnectionGuard();
  auto lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  return graph_service.LoadGraph(element_id);
}

auto EditorMiniGitProjectFixture::ReadJournalRecords(sl_element_id_t element_id,
                                                     std::string*    error)
    -> std::vector<MiniGitJournalRecord> {
  MiniGitJournal journal(journal_path(element_id));
  if (!journal.Load(error)) {
    return {};
  }
  return journal.records();
}

auto EditorMiniGitProjectFixture::CountStoredCommits(sl_element_id_t element_id) -> std::uint64_t {
  auto guard = storage_->GetDBController().GetConnectionGuard();
  auto lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  return graph_service.CountCommitsForRoot(root_id(element_id));
}

auto EditorMiniGitProjectFixture::RuntimeFor(sl_element_id_t element_id) -> ImageRuntime& {
  if (element_id == kElementA) {
    return image_a_;
  }
  if (element_id == kElementB) {
    return image_b_;
  }
  throw std::invalid_argument("EditorMiniGitProjectFixture: unknown element_id");
}

auto EditorMiniGitProjectFixture::RuntimeFor(sl_element_id_t element_id) const
    -> const ImageRuntime& {
  if (element_id == kElementA) {
    return image_a_;
  }
  if (element_id == kElementB) {
    return image_b_;
  }
  throw std::invalid_argument("EditorMiniGitProjectFixture: unknown element_id");
}

void EditorMiniGitProjectFixture::OpenProjectObjects() {
  project_      = std::make_unique<ProjectService>(db_path_, meta_path_);
  storage_      = project_->GetStorageService();
  materializer_ = std::make_unique<EditorMiniGitMaterializer>(storage_);
}

void EditorMiniGitProjectFixture::CloseProjectObjects() {
  image_a_.history.reset();
  image_b_.history.reset();
  image_a_.journal.reset();
  image_b_.journal.reset();
  image_a_.graph.reset();
  image_b_.graph.reset();
  materializer_.reset();
  storage_.reset();
  project_.reset();
}

void EditorMiniGitProjectFixture::CreatePersistedImage(ImageRuntime&       runtime,
                                                       sl_element_id_t     element_id,
                                                       const std::string&  version_name) {
  runtime.element_id   = element_id;
  runtime.journal_path = root_dir_ / ("image_" + std::to_string(element_id) + ".mini-git.wal");
  auto guard           = storage_->GetDBController().GetConnectionGuard();
  auto lock            = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               graph = graph_service.CreateEmptyPersisted(element_id, version_name);
  runtime.root_id          = graph.GetRootId();
  runtime.graph            = std::make_shared<CommitGraph>(std::move(graph));
}

void EditorMiniGitProjectFixture::RebuildWorkingRuntimes() {
  auto rebuild = [this](ImageRuntime& runtime) {
    auto stored = LoadStoredGraph(runtime.element_id);
    if (!stored.has_value()) {
      throw std::runtime_error("EditorMiniGitProjectFixture: missing stored graph");
    }
    runtime.root_id = stored->GetRootId();
    runtime.graph   = std::make_shared<CommitGraph>(std::move(*stored));
    runtime.journal = std::make_shared<MiniGitJournal>(runtime.journal_path);
    std::string error;
    if (std::filesystem::exists(runtime.journal_path) && !runtime.journal->Load(&error)) {
      throw std::runtime_error("EditorMiniGitProjectFixture: journal load failed: " + error);
    }
    runtime.history =
        std::make_unique<MiniGitWorkingHistory>(runtime.graph, runtime.journal);
  };
  rebuild(image_a_);
  rebuild(image_b_);
}

}  // namespace alcedo::test
