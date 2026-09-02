//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "support/editor_mini_git_project_fixture.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_clock_test_access.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "json.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::test {
namespace {

auto MakeExposureBatch(float before, float after) -> PipelineEditBatch {
  PipelineEditBatch batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = nlohmann::json{{"exposure_ev", before}};
  change.after_value                   = nlohmann::json{{"exposure_ev", after}};
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));
  return batch;
}

auto DocumentWithExposure(float exposure) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  auto* model   = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (model == nullptr) {
    throw std::runtime_error("EditorMiniGitProjectFixture: default document missing exposure");
  }
  model->SetValue(exposure);
  return document;
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

auto EditorMiniGitProjectFixture::CheckpointDocumentExposure(const nlohmann::json& checkpoint)
    -> float {
  const auto& nodes = checkpoint.at("pipeline_document").at("nodes");
  for (const auto& node : nodes) {
    if (node.at("id") != "grade.primary") {
      continue;
    }
    for (const auto& adjustment : node.at("adjustments")) {
      if (adjustment.at("type").get<std::string>() == type_ids::Exposure().Text()) {
        return adjustment.at("params").at("exposure_ev").get<float>();
      }
    }
  }
  throw std::runtime_error(
      "EditorMiniGitProjectFixture: checkpoint is missing Default Color Grade exposure");
}

auto EditorMiniGitProjectFixture::AppendExposureEdit(sl_element_id_t element_id, float before,
                                                     float after, std::string* error) -> bool {
  auto& runtime = RuntimeFor(element_id);
  auto  result  = runtime.history->AppendEdit(MakeExposureBatch(before, after));
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
  auto&      runtime  = RuntimeFor(element_id);
  const auto snapshot = runtime.journal->Snapshot();
  // Match production CaptureSaveCheckpoint: one materialization, then project
  // top-level identity fields from it (no independent working_head read).
  const auto logical_head  = runtime.graph->GetActiveVersionRef().head_commit_hash;
  const auto logical_chain = runtime.graph->ChainHashForHead(logical_head);
  const auto serialized    = MakeEditorSerializedPipelineState(
      runtime.graph->GetRootId(), logical_head, logical_chain, DocumentWithExposure(exposure));
  EditorMiniGitSaveCapture capture;
  capture.journal_records        = snapshot.records;
  capture.journal_path           = runtime.journal_path;
  capture.first_journal_sequence = snapshot.first_sequence;
  capture.last_journal_sequence  = snapshot.last_sequence;
  capture.materialization =
      runtime.graph->CaptureMaterializationWithSerializedPipelineState(serialized);
  capture.element_id             = capture.materialization.image_state.element_id;
  capture.version_id             = capture.materialization.image_state.active_version_id;
  capture.root_id                = capture.materialization.image_state.root_id;
  capture.working_head           = capture.materialization.image_state.materialized_head_commit_hash;
  capture.transaction_chain_hash =
      capture.materialization.image_state.materialized_transaction_chain_hash;
  return capture;
}

void EditorMiniGitProjectFixture::CloseAndReopenProject() {
  CloseProjectObjects();
  OpenProjectObjects();
  RebuildWorkingRuntimes();
}

auto EditorMiniGitProjectFixture::LoadStoredGraph(sl_element_id_t element_id)
    -> std::optional<CommitGraph> {
  auto guard = storage_->GetDatabase().GetConnectionGuard();
  auto lock  = guard.Lock();
  CommitGraphStore graph_service(guard.conn_);
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
  auto guard = storage_->GetDatabase().GetConnectionGuard();
  auto lock  = guard.Lock();
  CommitGraphStore graph_service(guard.conn_);
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
  project_          = std::make_unique<ProjectService>(db_path_, meta_path_);
  storage_          = project_->GetStorage();
  save_coordinator_ = std::make_shared<EditorSaveCheckpointCoordinator>();
  materializer_     = std::make_unique<EditorMiniGitMaterializer>(storage_, save_coordinator_);
}

void EditorMiniGitProjectFixture::CloseProjectObjects() {
  image_a_.history.reset();
  image_b_.history.reset();
  image_a_.journal.reset();
  image_b_.journal.reset();
  image_a_.graph.reset();
  image_b_.graph.reset();
  materializer_.reset();
  if (save_coordinator_) {
    save_coordinator_->Shutdown();
  }
  save_coordinator_.reset();
  storage_.reset();
  project_.reset();
}

auto EditorMiniGitProjectFixture::MaterializeUnderSaveLock(const EditorMiniGitSaveCapture& capture,
                                                           std::string* error)
    -> EditorMiniGitMaterializeResult {
  auto lock = save_coordinator_->AcquireBlocking(capture.element_id);
  if (!lock.owns_lock()) {
    if (error != nullptr) {
      *error = "save coordinator refused MaterializeUnderSaveLock";
    }
    EditorMiniGitMaterializeResult result;
    result.error = error != nullptr ? *error : "save lock unavailable";
    return result;
  }
  auto result = materializer_->Materialize(capture, error);
  // Match production EditorSessionService: after DuckDB commit the materializer
  // truncates the on-disk WAL file, then DiscardMaterializedJournalThrough drops
  // only the captured prefix from the live journal and rewrites any later
  // appends. TruncateMaterialized would also wipe those later records.
  if (result.accepted && result.materialized) {
    std::string discard_error;
    auto&       journal = *RuntimeFor(capture.element_id).journal;
    const bool discarded =
        capture.last_journal_sequence.has_value() && *capture.last_journal_sequence != 0
            ? journal.TruncateThroughSequence(*capture.last_journal_sequence, &discard_error)
            : journal.TruncateMaterialized(&discard_error);
    if (!discarded && error != nullptr && error->empty()) {
      *error = discard_error;
    }
  }
  return result;
}

void EditorMiniGitProjectFixture::CreatePersistedImage(ImageRuntime&       runtime,
                                                       sl_element_id_t     element_id,
                                                       const std::string&  version_name) {
  runtime.element_id   = element_id;
  runtime.journal_path = root_dir_ / ("image_" + std::to_string(element_id) + ".mini-git.wal");
  auto guard           = storage_->GetDatabase().GetConnectionGuard();
  auto lock            = guard.Lock();
  CommitGraphStore graph_service(guard.conn_);
  auto               graph = graph_service.CreateRootPipelinePersisted(
      element_id, CreateDefaultPipelineDocument(), std::nullopt, version_name);
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
