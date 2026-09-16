//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_node_graph_draft.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "grade_owned_mask_support.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId = 946;

struct ProjectPaths {
  std::filesystem::path root;
  std::filesystem::path database;
  std::filesystem::path metadata;
  std::filesystem::path journal;
  std::filesystem::path mask_root;
};

auto MakeProjectPaths() -> ProjectPaths {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  ProjectPaths paths;
  paths.root      = std::filesystem::path{"build/tmp"} / ("node_topology_history_" + stamp);
  paths.database  = paths.root / "project.db";
  paths.metadata  = paths.root / "project.json";
  paths.journal   = paths.root / "editor.wal";
  paths.mask_root = paths.root / "masks";
  std::filesystem::create_directories(paths.root);
  return paths;
}

class TemporaryProject final {
 public:
  TemporaryProject() : paths_(MakeProjectPaths()) {}
  ~TemporaryProject() {
    std::error_code error;
    std::filesystem::remove_all(paths_.root, error);
  }

  TemporaryProject(const TemporaryProject&)             = delete;
  TemporaryProject&  operator=(const TemporaryProject&) = delete;

  [[nodiscard]] auto paths() const -> const ProjectPaths& { return paths_; }

 private:
  ProjectPaths paths_;
};

class PersistentEditor final {
 public:
  PersistentEditor(const ProjectPaths& paths, ProjectOpenMode mode, sl_element_id_t element_id)
      : paths_(paths),
        project_(paths.database, paths.metadata, mode),
        pipeline_service_(std::make_shared<PipelineMgmtService>(project_.GetStorage())),
        pipeline_(std::make_shared<EditorSessionPipelinePort>()),
        element_id_(element_id) {}

  auto Open(std::string* error) -> bool {
    try {
      guard_ = pipeline_service_->LoadEditorPipeline(element_id_);
      if (guard_ == nullptr) {
        if (error != nullptr) {
          *error = "PipelineMgmtService returned no editor guard";
        }
        return false;
      }
      pipeline_->SetServices(
          EditorSessionPipelineMappers{[service = pipeline_service_]() { return service; },
                                       [guard = guard_](sl_element_id_t) { return guard; }});
      history_.SetServices(EditorSessionHistoryPort::Services{
          [journal = paths_.journal](sl_element_id_t) { return journal; }});
      history_.SetPipelinePort(pipeline_);
      handle_ = history_.Acquire(element_id_, error);
      return handle_.valid;
    } catch (const std::exception& exception) {
      if (error != nullptr) {
        *error = exception.what();
      }
      return false;
    }
  }

  void ReleaseHistory() {
    if (!handle_.valid) {
      return;
    }
    history_.Release(handle_);
    handle_ = {};
  }

  void SavePipelineAndMetadata() {
    ReleaseHistory();
    pipeline_service_->SavePipeline(guard_);
    project_.SaveProject(paths_.metadata);
  }

  void SaveMetadataOnly() {
    ReleaseHistory();
    project_.SaveProject(paths_.metadata);
  }

  auto MaterializeCheckpoint(std::string* error) -> bool {
    const auto capture = history_.CaptureSaveCheckpoint(handle_, error);
    if (!capture || !capture->last_journal_sequence.has_value()) {
      return false;
    }
    {
      auto             db_guard = project_.GetStorage()->GetDatabase().GetConnectionGuard();
      auto             db_lock  = db_guard.Lock();
      CommitGraphStore graph_service(db_guard.conn_);
      graph_service.Materialize(capture->materialization);
    }
    if (!history_.DiscardMaterializedJournalThrough(handle_, *capture->last_journal_sequence,
                                                    error)) {
      return false;
    }
    return history_.SyncMaterializedStateAfterCheckpoint(handle_, error);
  }

  [[nodiscard]] auto guard() const -> const std::shared_ptr<PipelineGuard>& { return guard_; }
  [[nodiscard]] auto guard() -> std::shared_ptr<PipelineGuard>& { return guard_; }
  [[nodiscard]] auto history() -> EditorSessionHistoryPort& { return history_; }
  [[nodiscard]] auto handle() const -> const EditorHistoryGuardHandle& { return handle_; }

 private:
  ProjectPaths                               paths_;
  ProjectService                             project_;
  std::shared_ptr<PipelineMgmtService>       pipeline_service_;
  std::shared_ptr<PipelineGuard>             guard_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
  EditorHistoryGuardHandle                   handle_{};
  sl_element_id_t                            element_id_ = 0;
};

struct TopologyEditExpectation {
  NodeGraphTopologyChange        change;
  std::vector<NodeId>            node_ids;
  std::vector<PipelineSceneEdge> edges;
};

TEST(NodeGraphTopologyHistory, DefaultGradeRemovalClearsIdentityAndUndoRestoresIt) {
  auto document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->SetDeletionProtected(false);
  const auto default_id = document.DefaultGradeId();
  const auto before = document.ToJson();
  auto draft = EditorNodeGraphDraft::FromDocument(document, {});
  ASSERT_TRUE(draft.RemoveColorGrade(document, default_id).succeeded);
  ASSERT_TRUE(draft.Connect(NodeId{"develop"}, NodeId{"drt"}).succeeded);
  ASSERT_TRUE(draft.SubmissionValid());
  const auto batch = MakeEditNodeGraphBatch(draft.MakeChange());
  const auto stored = PipelineEditBatch::FromJSON(batch.CanonicalJSON());
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, stored, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_TRUE(document.DefaultGradeId().Empty());
  EXPECT_EQ(document.Graph().FindNode(default_id), nullptr);
  EXPECT_NO_THROW((void)PipelineDocument::FromJson(document.ToJson()));
  ASSERT_TRUE(ApplyPipelineEditBatch(document, stored, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(document.DefaultGradeId(), default_id);
  EXPECT_EQ(document.ToJson(), before);
}

auto BuildTopologyEdit(const PipelineDocument& document) -> TopologyEditExpectation {
  auto       draft   = EditorNodeGraphDraft::FromDocument(document, {});
  const auto require = [](const EditorNodeGraphDraftMutation& mutation, const char* operation) {
    if (!mutation.succeeded) {
      throw std::runtime_error(std::string{operation} + ": " + mutation.error);
    }
  };

  const NodeId extra{"grade.topology"};
  require(draft.AddColorGrade(extra), "add color grade");
  require(draft.Connect(NodeId{"develop"}, extra), "connect develop");
  require(draft.Connect(extra, NodeId{"grade.primary"}), "connect primary input");
  require(draft.Connect(NodeId{"grade.primary"}, NodeId{"drt"}), "connect drt");
  if (!draft.SubmissionValid()) {
    throw std::runtime_error("topology draft is not a complete Develop-to-DRT path");
  }

  TopologyEditExpectation expected;
  expected.change = draft.MakeChange();
  expected.node_ids.reserve(draft.Nodes().size());
  for (const auto& node : draft.Nodes()) {
    expected.node_ids.push_back(node.node_id);
  }
  expected.edges.reserve(draft.Edges().size());
  for (const auto& edge : draft.Edges()) {
    expected.edges.push_back(PipelineSceneEdge{edge.source_node_id, edge.source_port_id,
                                               edge.destination_node_id, edge.destination_port_id});
  }
  return expected;
}

auto GraphNodeIds(const PipelineDocument& document) -> std::vector<NodeId> {
  std::vector<NodeId> ids;
  ids.reserve(document.Graph().Nodes().size());
  for (const auto& node : document.Graph().Nodes()) {
    ids.push_back(node->Id());
  }
  return ids;
}

auto GraphEdges(const PipelineDocument& document) -> std::vector<PipelineSceneEdge> {
  std::vector<PipelineSceneEdge> edges;
  edges.reserve(document.Graph().Edges().size());
  for (const auto& edge : document.Graph().Edges()) {
    edges.push_back(PipelineSceneEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port});
  }
  return edges;
}

void ExpectGraphOrder(const PipelineDocument& document, const std::vector<NodeId>& node_ids,
                      const std::vector<PipelineSceneEdge>& edges) {
  ASSERT_EQ(GraphNodeIds(document), node_ids);
  EXPECT_EQ(GraphEdges(document), edges);
}

auto MaskJson(const PipelineDocument& document, const MaskId& mask_id) -> nlohmann::json {
  const auto* grade = document.Graph().FindNode(NodeId{"grade.primary"});
  const auto* model = dynamic_cast<const ColorGradeNodeModel*>(grade);
  if (model == nullptr) {
    throw std::runtime_error("primary Color Grade is missing");
  }
  const auto* mask = model->FindMask(mask_id);
  if (mask == nullptr) {
    throw std::runtime_error("expected persistent Mask is missing");
  }
  return MaskModelToJson(*mask);
}

auto MakeMemoryGuard(sl_element_id_t element_id) -> std::shared_ptr<PipelineGuard> {
  auto guard           = std::make_shared<PipelineGuard>();
  guard->id_           = element_id;
  guard->pipeline_     = std::make_shared<CPUPipelineExecutor>();
  guard->document_     = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  guard->commit_graph_ = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(element_id));
  guard->root_id_      = guard->commit_graph_->GetRootId();
  guard->root_document_ =
      std::make_shared<PipelineDocument>(ClonePipelineDocument(*guard->document_));
  return guard;
}

TEST(NodeGraphTopologyHistory, ProductionPortPersistsExactTopologyThroughRecoveryAndCheckout) {
  RegisterAllOperators();
  TemporaryProject               temporary;
  const auto&                    paths = temporary.paths();
  std::string                    error;
  version_ref_id_t               default_version{};
  version_ref_id_t               second_version{};
  head_commit_hash_t             topology_head;
  head_commit_hash_t             masked_head;
  std::string                    before_topology_hash;
  std::string                    after_topology_hash;
  std::string                    after_mask_hash;
  std::vector<NodeId>            initial_nodes;
  std::vector<PipelineSceneEdge> initial_edges;
  std::vector<NodeId>            topology_nodes;
  std::vector<PipelineSceneEdge> topology_edges;
  nlohmann::json                 expected_mask;

  {
    PersistentEditor editor(paths, ProjectOpenMode::kCreateNew, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    auto& guard          = *editor.guard();
    default_version      = guard.commit_graph_->GetActiveVersionId();
    initial_nodes        = GraphNodeIds(*guard.document_);
    initial_edges        = GraphEdges(*guard.document_);

    before_topology_hash = CanonicalPipelineDocumentJson(*guard.document_);
    const auto topology  = BuildTopologyEdit(*guard.document_);
    topology_nodes       = topology.node_ids;
    topology_edges       = topology.edges;
    ASSERT_TRUE(editor.history().EditNodeGraph(editor.handle(), topology.change, &error)) << error;
    after_topology_hash = CanonicalPipelineDocumentJson(*guard.document_);
    topology_head       = guard.working_head_commit_hash();

    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), default_version);
    EXPECT_EQ(guard.document_->NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(*guard.document_, topology_nodes, topology_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(),
              EditorRenderReason::GraphTopologyChanged);

    EditorHistorySnapshot snapshot;
    ASSERT_TRUE(editor.history().ReadHistorySnapshot(editor.handle(), &snapshot, &error)) << error;
    ASSERT_FALSE(snapshot.commits.empty());
    EXPECT_EQ(snapshot.active_version_id, default_version);
    EXPECT_EQ(snapshot.active_head, topology_head);
    const auto topology_commit =
        std::find_if(snapshot.commits.begin(), snapshot.commits.end(),
                     [](const auto& commit) { return commit.operation_kind == "edit_node_graph"; });
    ASSERT_NE(topology_commit, snapshot.commits.end());
    EXPECT_EQ(topology_commit->commit_hash, topology_head.value());
    EXPECT_TRUE(snapshot.can_undo);
    EXPECT_FALSE(snapshot.can_redo);

    ASSERT_TRUE(editor.history().Undo(editor.handle(), &error)) << error;
    EXPECT_EQ(CanonicalPipelineDocumentJson(*guard.document_), before_topology_hash);
    EXPECT_EQ(guard.document_->NextColorGradeNameNumber(), 2u);
    ExpectGraphOrder(*guard.document_, initial_nodes, initial_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(), EditorRenderReason::UndoRedo);

    ASSERT_TRUE(editor.history().Redo(editor.handle(), &error)) << error;
    EXPECT_EQ(CanonicalPipelineDocumentJson(*guard.document_), after_topology_hash);
    EXPECT_EQ(guard.document_->NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(*guard.document_, topology_nodes, topology_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(), EditorRenderReason::UndoRedo);

    editor.SaveMetadataOnly();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    const auto& guard = *editor.guard();
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), default_version);
    EXPECT_EQ(guard.commit_graph_->GetAllVersionRefs().size(), 1u);
    EXPECT_EQ(guard.working_head_commit_hash(), topology_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(*guard.document_), after_topology_hash);
    EXPECT_EQ(guard.document_->NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(*guard.document_, topology_nodes, topology_edges);

    EditorHistorySnapshot snapshot;
    ASSERT_TRUE(editor.history().ReadHistorySnapshot(editor.handle(), &snapshot, &error)) << error;
    EXPECT_EQ(snapshot.active_version_id, default_version);
    EXPECT_EQ(snapshot.active_head, topology_head);
    ASSERT_FALSE(snapshot.commits.empty());
    EXPECT_TRUE(
        std::any_of(snapshot.commits.begin(), snapshot.commits.end(),
                    [](const auto& commit) { return commit.operation_kind == "edit_node_graph"; }));

    ASSERT_TRUE(editor.history().CreateRootVersionAndCheckout(editor.handle(), "Clean",
                                                              &second_version, &error))
        << error;
    EXPECT_NE(second_version, default_version);
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), second_version);
    EXPECT_FALSE(guard.working_head_commit_hash().has_value());
    ExpectGraphOrder(*guard.document_, initial_nodes, initial_edges);

    ASSERT_TRUE(editor.history().CheckoutVersion(editor.handle(), default_version, &error))
        << error;
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), default_version);
    EXPECT_EQ(guard.working_head_commit_hash(), topology_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(*guard.document_), after_topology_hash);
    ExpectGraphOrder(*guard.document_, topology_nodes, topology_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(),
              EditorRenderReason::VersionDocumentChanged);

    auto mask = grade_mask_test::MakeRadialMask(MaskId{"mask.topology"});
    mask.deletion_protected = true;
    expected_mask = MaskModelToJson(mask);
    ASSERT_TRUE(editor.history().AddMask(editor.handle(), NodeId{"grade.primary"}, mask, 0, &error))
        << error;
    EXPECT_EQ(MaskJson(*guard.document_, MaskId{"mask.topology"}), expected_mask);
    after_mask_hash = CanonicalPipelineDocumentJson(*guard.document_);
    masked_head     = guard.working_head_commit_hash();
    ASSERT_TRUE(editor.MaterializeCheckpoint(&error)) << error;
    editor.SavePipelineAndMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    const auto& guard = *editor.guard();
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), default_version);
    EXPECT_EQ(guard.working_head_commit_hash(), masked_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(*guard.document_), after_mask_hash);
    EXPECT_EQ(guard.commit_graph_->GetAllVersionRefs().size(), 2u);
    ExpectGraphOrder(*guard.document_, topology_nodes, topology_edges);
    EXPECT_EQ(MaskJson(*guard.document_, MaskId{"mask.topology"}), expected_mask);

    EditorHistorySnapshot snapshot;
    ASSERT_TRUE(editor.history().ReadHistorySnapshot(editor.handle(), &snapshot, &error)) << error;
    EXPECT_EQ(snapshot.active_version_id, default_version);
    EXPECT_EQ(snapshot.active_head, masked_head);
    ASSERT_FALSE(snapshot.commits.empty());
    EXPECT_TRUE(
        std::any_of(snapshot.commits.begin(), snapshot.commits.end(),
                    [](const auto& commit) { return commit.operation_kind == "edit_node_graph"; }));

    ASSERT_TRUE(editor.history().CheckoutVersion(editor.handle(), second_version, &error)) << error;
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), second_version);
    EXPECT_FALSE(guard.working_head_commit_hash().has_value());
    ExpectGraphOrder(*guard.document_, initial_nodes, initial_edges);
    EXPECT_EQ(guard.document_->NextColorGradeNameNumber(), 2u);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(),
              EditorRenderReason::VersionDocumentChanged);
  }
}

TEST(NodeGraphTopologyHistory, ProductionPortRecoversExplicitNodeAndMaskUnlockFromCheckpointAndWal) {
  RegisterAllOperators();
  TemporaryProject   temporary;
  const auto&        paths = temporary.paths();
  const NodeId       grade_id{"grade.primary"};
  const MaskId       mask_id{"mask.persistent_unlock"};
  std::string        error;
  version_ref_id_t   default_version{};
  head_commit_hash_t locked_head;
  head_commit_hash_t node_unlocked_head;
  head_commit_hash_t both_unlocked_head;

  const auto expect_state = [&](PersistentEditor& editor, const head_commit_hash_t& head,
                                bool node_protected, bool mask_protected) {
    const auto& guard = *editor.guard();
    EXPECT_EQ(guard.document_->DefaultGradeId(), grade_id);
    EXPECT_EQ(guard.commit_graph_->GetActiveVersionId(), default_version);
    EXPECT_EQ(guard.working_head_commit_hash(), head);
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
        guard.document_->Graph().FindNode(grade_id));
    ASSERT_NE(grade, nullptr);
    EXPECT_EQ(grade->DeletionProtected(), node_protected);
    ASSERT_EQ(grade->Masks().size(), 1u);
    const auto* mask = grade->FindMask(mask_id);
    ASSERT_NE(mask, nullptr);
    EXPECT_EQ(mask->id, mask_id);
    EXPECT_EQ(mask->deletion_protected, mask_protected);

    EditorHistorySnapshot snapshot;
    ASSERT_TRUE(editor.history().ReadHistorySnapshot(editor.handle(), &snapshot, &error)) << error;
    EXPECT_EQ(snapshot.active_version_id, default_version);
    EXPECT_EQ(snapshot.active_head, head);
  };

  {
    PersistentEditor editor(paths, ProjectOpenMode::kCreateNew, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    default_version = editor.guard()->commit_graph_->GetActiveVersionId();
    ASSERT_TRUE(editor.history().AddMask(editor.handle(), grade_id,
                                         grade_mask_test::MakeRadialMask(mask_id), 0, &error))
        << error;
    locked_head = editor.guard()->working_head_commit_hash();
    ASSERT_TRUE(locked_head.has_value());
    expect_state(editor, locked_head, true, true);

    ASSERT_TRUE(editor.history().SetColorGradeDeletionProtected(editor.handle(), grade_id, false,
                                                               &error))
        << error;
    node_unlocked_head = editor.guard()->working_head_commit_hash();
    ASSERT_TRUE(node_unlocked_head.has_value());
    EXPECT_NE(node_unlocked_head, locked_head);
    expect_state(editor, node_unlocked_head, false, true);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    ASSERT_TRUE(editor.MaterializeCheckpoint(&error)) << error;
    editor.SavePipelineAndMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    expect_state(editor, node_unlocked_head, false, true);

    ASSERT_TRUE(editor.history().SetMaskField(editor.handle(), grade_id, mask_id,
                                             "deletion_protected", false, &error))
        << error;
    both_unlocked_head = editor.guard()->working_head_commit_hash();
    ASSERT_TRUE(both_unlocked_head.has_value());
    EXPECT_NE(both_unlocked_head, node_unlocked_head);
    expect_state(editor, both_unlocked_head, false, false);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    // Leave the Mask unlock in the WAL rather than saving its document to the database.
    editor.SaveMetadataOnly();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    expect_state(editor, both_unlocked_head, false, false);

    ASSERT_TRUE(editor.history().Undo(editor.handle(), &error)) << error;
    expect_state(editor, node_unlocked_head, false, true);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    ASSERT_TRUE(editor.history().Undo(editor.handle(), &error)) << error;
    expect_state(editor, locked_head, true, true);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());

    ASSERT_TRUE(editor.history().Redo(editor.handle(), &error)) << error;
    expect_state(editor, node_unlocked_head, false, true);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    ASSERT_TRUE(editor.history().Redo(editor.handle(), &error)) << error;
    expect_state(editor, both_unlocked_head, false, false);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    ASSERT_TRUE(editor.MaterializeCheckpoint(&error)) << error;
    editor.SavePipelineAndMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    expect_state(editor, both_unlocked_head, false, false);
  }
}

TEST(NodeGraphTopologyHistory, JournalFailureRestoresTopologyDocumentHeadAndRenderState) {
  RegisterAllOperators();
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             guard    = MakeMemoryGuard(kElementId + 1);
  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(
      EditorSessionPipelineMappers{{}, [guard](sl_element_id_t) { return guard; }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 1, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(std::filesystem::create_directory(paths.journal));
  const auto prior_hash         = CanonicalPipelineDocumentJson(*guard->document_);
  const auto prior_head         = guard->working_head_commit_hash();
  const auto prior_commit_count = guard->commit_graph_->CommitCount();
  const auto prior_reason       = history.LastPublishedRenderReason();
  const auto prior_counter      = guard->document_->NextColorGradeNameNumber();

  const auto topology           = BuildTopologyEdit(*guard->document_);
  error.clear();
  EXPECT_FALSE(history.EditNodeGraph(handle, topology.change, &error));
  EXPECT_EQ(error, "mini-Git journal file could not be opened for append");
  EXPECT_EQ(CanonicalPipelineDocumentJson(*guard->document_), prior_hash);
  EXPECT_EQ(guard->working_head_commit_hash(), prior_head);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  const std::vector<NodeId> expected_initial_ids = {NodeId{"develop"}, NodeId{"grade.primary"},
                                                    NodeId{"drt"}};
  EXPECT_EQ(GraphNodeIds(*guard->document_), expected_initial_ids);
  history.Release(handle);
}

TEST(NodeGraphTopologyHistory, MaskGroupTopInsertAndBridgeRemoveCommitOnceAndReplayThroughUndo) {
  RegisterAllOperators();
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             guard    = MakeMemoryGuard(kElementId + 2);
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(
      guard->document_->Graph().FindNode(NodeId{"grade.primary"}));
  ASSERT_NE(grade, nullptr);
  grade->SetDeletionProtected(false);
  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(
      EditorSessionPipelineMappers{{}, [guard](sl_element_id_t) { return guard; }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 2, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto prior_hash         = CanonicalPipelineDocumentJson(*guard->document_);
  const auto prior_head         = guard->working_head_commit_hash();
  const auto prior_commit_count = guard->commit_graph_->CommitCount();

  // Stale-successor rejection: the node after Develop is grade.primary, not drt.
  EXPECT_FALSE(history.InsertColorGradeAtTop(handle, NodeId{"grade.stale"}, NodeId{"drt"}, &error));
  EXPECT_EQ(error, "The Mask Groups insertion point changed since the request was issued");
  EXPECT_EQ(CanonicalPipelineDocumentJson(*guard->document_), prior_hash);
  EXPECT_EQ(guard->working_head_commit_hash(), prior_head);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(guard->document_->Graph().FindNode(NodeId{"grade.stale"}), nullptr);

  // One typed commit inserts the clean grade between Develop and grade.primary.
  error.clear();
  ASSERT_TRUE(
      history.InsertColorGradeAtTop(handle, NodeId{"grade.top"}, NodeId{"grade.primary"}, &error))
      << error;
  const std::vector<NodeId> inserted_backbone = {NodeId{"develop"}, NodeId{"grade.top"},
                                                 NodeId{"grade.primary"}, NodeId{"drt"}};
  EXPECT_EQ(guard->document_->Graph().ImageBackboneNodeIds(), inserted_backbone);
  EXPECT_EQ(guard->document_->Graph().FindNode(NodeId{"grade.top"})->DisplayName(),
            "Color Grade 2");
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count + 1);
  ASSERT_TRUE(history.LastPublishedRenderReason().has_value());
  EXPECT_EQ(*history.LastPublishedRenderReason(), EditorRenderReason::GraphTopologyChanged);
  const auto inserted_hash = CanonicalPipelineDocumentJson(*guard->document_);
  const auto inserted_top_json =
      guard->document_->Graph().FindNode(NodeId{"grade.top"})->ToJson().dump();

  EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(std::any_of(snapshot.commits.begin(), snapshot.commits.end(), [](const auto& commit) {
    return commit.operation_kind == "add_color_grade";
  }));

  // Undo removes the node; Redo restores the exact stored state.
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  EXPECT_EQ(guard->document_->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(guard->document_->Graph().FindNode(NodeId{"grade.top"}), nullptr);
  ASSERT_TRUE(history.Redo(handle, &error)) << error;
  EXPECT_EQ(CanonicalPipelineDocumentJson(*guard->document_), inserted_hash);

  // Bridge-remove the inserted grade: one commit, predecessor wired to successor.
  error.clear();
  ASSERT_TRUE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.top"}, &error)) << error;
  EXPECT_EQ(guard->document_->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_NE(alcedo::FindSceneImageEdge(guard->document_->Graph(), NodeId{"develop"},
                                       NodeId{"grade.primary"}),
            nullptr);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count + 2);

  // Removing the last remaining Color Grade leaves Develop connected to DRT.
  error.clear();
  ASSERT_TRUE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.primary"}, &error)) << error;
  EXPECT_EQ(guard->document_->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"drt"}}));
  EXPECT_NE(alcedo::FindSceneImageEdge(guard->document_->Graph(), NodeId{"develop"}, NodeId{"drt"}),
            nullptr);

  // Endpoints are never removable; failures publish no commit.
  const auto commits_before_endpoint = guard->commit_graph_->CommitCount();
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"develop"}, &error));
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"drt"}, &error));
  EXPECT_EQ(guard->commit_graph_->CommitCount(), commits_before_endpoint);

  // Both removals undo back to the stored post-insert topology. Node and edge
  // container positions are not part of the stored typed change, so the check is
  // semantic: identical backbone order and identical restored node JSON.
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  EXPECT_EQ(guard->document_->Graph().ImageBackboneNodeIds(), inserted_backbone);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(guard->document_->Graph().FindNode(NodeId{"grade.top"})->ToJson().dump(),
            inserted_top_json);
  history.Release(handle);
}

TEST(NodeGraphTopologyHistory, MaskGroupJournalFailureLeavesDocumentHeadAndCounterUntouched) {
  RegisterAllOperators();
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             guard    = MakeMemoryGuard(kElementId + 3);
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(
      guard->document_->Graph().FindNode(NodeId{"grade.primary"}));
  ASSERT_NE(grade, nullptr);
  grade->SetDeletionProtected(false);
  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(
      EditorSessionPipelineMappers{{}, [guard](sl_element_id_t) { return guard; }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 3, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(std::filesystem::create_directory(paths.journal));
  const auto prior_hash         = CanonicalPipelineDocumentJson(*guard->document_);
  const auto prior_head         = guard->working_head_commit_hash();
  const auto prior_commit_count = guard->commit_graph_->CommitCount();
  const auto prior_reason       = history.LastPublishedRenderReason();
  const auto prior_counter      = guard->document_->NextColorGradeNameNumber();

  EXPECT_FALSE(
      history.InsertColorGradeAtTop(handle, NodeId{"grade.top"}, NodeId{"grade.primary"}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(CanonicalPipelineDocumentJson(*guard->document_), prior_hash);
  EXPECT_EQ(guard->working_head_commit_hash(), prior_head);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(guard->document_->Graph().FindNode(NodeId{"grade.top"}), nullptr);

  error.clear();
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.primary"}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(CanonicalPipelineDocumentJson(*guard->document_), prior_hash);
  EXPECT_EQ(guard->working_head_commit_hash(), prior_head);
  EXPECT_EQ(guard->commit_graph_->CommitCount(), prior_commit_count);
  EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  history.Release(handle);
}

}  // namespace
}  // namespace alcedo::ui
