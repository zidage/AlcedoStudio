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
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
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
  /// A new project (@p mode kCreateNew) also receives the history root of @p element_id, as an
  /// import creates it.
  PersistentEditor(const ProjectPaths& paths, ProjectOpenMode mode, sl_element_id_t element_id)
      : paths_(paths),
        project_(paths.database, paths.metadata, mode),
        pipeline_service_(std::make_shared<PipelineMgmtService>(project_.GetStorage())),
        pipeline_(std::make_shared<EditorSessionPipelinePort>()),
        element_id_(element_id) {
    if (mode == ProjectOpenMode::kCreateNew) {
      pipeline_service_->InitializeImageRoot(element_id_, CreateDefaultPipelineDocument(), nullptr);
    }
  }

  auto Open(std::string* error) -> bool {
    try {
      pipeline_->SetServices(
          EditorSessionPipelineMappers{[service = pipeline_service_]() { return service; }, {}});
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

  /// Return the editor lease and save the project metadata. Journal records that were not
  /// materialized stay in the WAL for the next open.
  void ReleaseAndSaveMetadata() {
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

  /// Copy of the editor's CommitGraph.
  [[nodiscard]] auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(history_, element_id_);
  }
  [[nodiscard]] auto Head() -> head_commit_hash_t {
    return test::EditorWorkingHead(history_, element_id_);
  }
  /// Working document as the history published it after its last operation.
  [[nodiscard]] auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, element_id_);
  }
  [[nodiscard]] auto history() -> EditorSessionHistoryPort& { return history_; }
  [[nodiscard]] auto handle() const -> const EditorHistoryGuardHandle& { return handle_; }

 private:
  ProjectPaths                               paths_;
  ProjectService                             project_;
  std::shared_ptr<PipelineMgmtService>       pipeline_service_;
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
  auto draft = EditorNodeGraphDraft::FromDocument(document);
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
  auto       draft   = EditorNodeGraphDraft::FromDocument(document);
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

/// Default document whose primary Color Grade is not deletion protected.
auto UnprotectedPrimaryGradeDocument() -> PipelineDocument {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  if (grade == nullptr) {
    throw std::runtime_error("primary Color Grade is missing");
  }
  grade->SetDeletionProtected(false);
  return document;
}

TEST(NodeGraphTopologyHistory, ProductionPortPersistsExactTopologyThroughRecoveryAndCheckout) {
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
    default_version      = editor.Graph()->GetActiveVersionId();
    initial_nodes        = GraphNodeIds(editor.Working()->Document());
    initial_edges        = GraphEdges(editor.Working()->Document());

    before_topology_hash = CanonicalPipelineDocumentJson(editor.Working()->Document());
    const auto topology  = BuildTopologyEdit(editor.Working()->Document());
    topology_nodes       = topology.node_ids;
    topology_edges       = topology.edges;
    ASSERT_TRUE(editor.history().EditNodeGraph(editor.handle(), topology.change, &error)) << error;
    after_topology_hash = CanonicalPipelineDocumentJson(editor.Working()->Document());
    topology_head       = editor.Head();

    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), default_version);
    EXPECT_EQ(editor.Working()->Document().NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(editor.Working()->Document(), topology_nodes, topology_edges);
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
    EXPECT_EQ(CanonicalPipelineDocumentJson(editor.Working()->Document()), before_topology_hash);
    EXPECT_EQ(editor.Working()->Document().NextColorGradeNameNumber(), 2u);
    ExpectGraphOrder(editor.Working()->Document(), initial_nodes, initial_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(), EditorRenderReason::UndoRedo);

    ASSERT_TRUE(editor.history().Redo(editor.handle(), &error)) << error;
    EXPECT_EQ(CanonicalPipelineDocumentJson(editor.Working()->Document()), after_topology_hash);
    EXPECT_EQ(editor.Working()->Document().NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(editor.Working()->Document(), topology_nodes, topology_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(), EditorRenderReason::UndoRedo);

    editor.ReleaseAndSaveMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), default_version);
    EXPECT_EQ(editor.Graph()->GetAllVersionRefs().size(), 1u);
    EXPECT_EQ(editor.Head(), topology_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(editor.Working()->Document()), after_topology_hash);
    EXPECT_EQ(editor.Working()->Document().NextColorGradeNameNumber(), 3u);
    ExpectGraphOrder(editor.Working()->Document(), topology_nodes, topology_edges);

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
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), second_version);
    EXPECT_FALSE(editor.Head().has_value());
    ExpectGraphOrder(editor.Working()->Document(), initial_nodes, initial_edges);

    ASSERT_TRUE(editor.history().CheckoutVersion(editor.handle(), default_version, &error))
        << error;
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), default_version);
    EXPECT_EQ(editor.Head(), topology_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(editor.Working()->Document()), after_topology_hash);
    ExpectGraphOrder(editor.Working()->Document(), topology_nodes, topology_edges);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(),
              EditorRenderReason::VersionDocumentChanged);

    auto mask = grade_mask_test::MakeRadialMask(MaskId{"mask.topology"});
    mask.deletion_protected = true;
    expected_mask = MaskModelToJson(mask);
    ASSERT_TRUE(editor.history().AddMask(editor.handle(), NodeId{"grade.primary"}, mask, 0, &error))
        << error;
    EXPECT_EQ(MaskJson(editor.Working()->Document(), MaskId{"mask.topology"}), expected_mask);
    after_mask_hash = CanonicalPipelineDocumentJson(editor.Working()->Document());
    masked_head     = editor.Head();
    ASSERT_TRUE(editor.MaterializeCheckpoint(&error)) << error;
    editor.ReleaseAndSaveMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), default_version);
    EXPECT_EQ(editor.Head(), masked_head);
    EXPECT_EQ(CanonicalPipelineDocumentJson(editor.Working()->Document()), after_mask_hash);
    EXPECT_EQ(editor.Graph()->GetAllVersionRefs().size(), 2u);
    ExpectGraphOrder(editor.Working()->Document(), topology_nodes, topology_edges);
    EXPECT_EQ(MaskJson(editor.Working()->Document(), MaskId{"mask.topology"}), expected_mask);

    EditorHistorySnapshot snapshot;
    ASSERT_TRUE(editor.history().ReadHistorySnapshot(editor.handle(), &snapshot, &error)) << error;
    EXPECT_EQ(snapshot.active_version_id, default_version);
    EXPECT_EQ(snapshot.active_head, masked_head);
    ASSERT_FALSE(snapshot.commits.empty());
    EXPECT_TRUE(
        std::any_of(snapshot.commits.begin(), snapshot.commits.end(),
                    [](const auto& commit) { return commit.operation_kind == "edit_node_graph"; }));

    ASSERT_TRUE(editor.history().CheckoutVersion(editor.handle(), second_version, &error)) << error;
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), second_version);
    EXPECT_FALSE(editor.Head().has_value());
    ExpectGraphOrder(editor.Working()->Document(), initial_nodes, initial_edges);
    EXPECT_EQ(editor.Working()->Document().NextColorGradeNameNumber(), 2u);
    ASSERT_TRUE(editor.history().LastPublishedRenderReason().has_value());
    EXPECT_EQ(*editor.history().LastPublishedRenderReason(),
              EditorRenderReason::VersionDocumentChanged);
  }
}

TEST(NodeGraphTopologyHistory, ProductionPortRecoversExplicitNodeAndMaskUnlockFromCheckpointAndWal) {
  TemporaryProject   temporary;
  const auto&        paths = temporary.paths();
  const NodeId       grade_id{"grade.primary"};
  const MaskId       mask_id{"mask.persistent_unlock"};
  std::string        error;
  version_ref_id_t   default_version{};
  head_commit_hash_t locked_head;
  head_commit_hash_t node_unlocked_head;
  head_commit_hash_t both_unlocked_head;

  const auto         expect_state = [&](PersistentEditor& editor, const head_commit_hash_t& head,
                                bool node_protected, bool mask_protected) {
    const auto working = editor.Working();
    ASSERT_NE(working, nullptr);
    EXPECT_EQ(working->Document().DefaultGradeId(), grade_id);
    EXPECT_EQ(editor.Graph()->GetActiveVersionId(), default_version);
    EXPECT_EQ(editor.Head(), head);
    const auto* grade =
        dynamic_cast<const ColorGradeNodeModel*>(working->Document().Graph().FindNode(grade_id));
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
    default_version = editor.Graph()->GetActiveVersionId();
    ASSERT_TRUE(editor.history().AddMask(editor.handle(), grade_id,
                                         grade_mask_test::MakeRadialMask(mask_id), 0, &error))
        << error;
    // Masks are created unlocked; the persisted flag is still writable through
    // the history port so the checkpoint/WAL round-trip keeps covering it.
    ASSERT_TRUE(editor.history().SetMaskField(editor.handle(), grade_id, mask_id,
                                             "deletion_protected", true, &error))
        << error;
    locked_head = editor.Head();
    ASSERT_TRUE(locked_head.has_value());
    expect_state(editor, locked_head, true, true);

    ASSERT_TRUE(editor.history().SetColorGradeDeletionProtected(editor.handle(), grade_id, false,
                                                               &error))
        << error;
    node_unlocked_head = editor.Head();
    ASSERT_TRUE(node_unlocked_head.has_value());
    EXPECT_NE(node_unlocked_head, locked_head);
    expect_state(editor, node_unlocked_head, false, true);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    ASSERT_TRUE(editor.MaterializeCheckpoint(&error)) << error;
    editor.ReleaseAndSaveMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    expect_state(editor, node_unlocked_head, false, true);

    ASSERT_TRUE(editor.history().SetMaskField(editor.handle(), grade_id, mask_id,
                                             "deletion_protected", false, &error))
        << error;
    both_unlocked_head = editor.Head();
    ASSERT_TRUE(both_unlocked_head.has_value());
    EXPECT_NE(both_unlocked_head, node_unlocked_head);
    expect_state(editor, both_unlocked_head, false, false);
    EXPECT_FALSE(editor.history().LastPublishedRenderReason().has_value());
    // Leave the Mask unlock in the WAL rather than saving its document to the database.
    editor.ReleaseAndSaveMetadata();
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
    editor.ReleaseAndSaveMetadata();
  }

  {
    PersistentEditor editor(paths, ProjectOpenMode::kLoadExisting, kElementId);
    ASSERT_TRUE(editor.Open(&error)) << error;
    expect_state(editor, both_unlocked_head, false, false);
  }
}

TEST(NodeGraphTopologyHistory, JournalFailureRestoresTopologyDocumentHeadAndRenderState) {
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(EditorSessionPipelineMappers{
      {}, [](sl_element_id_t id) { return test::MakeInMemoryEditorLease(id); }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 1, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto graph_of = [&] { return test::EditorHistoryGraph(history, kElementId + 1); };
  const auto head_of  = [&] { return test::EditorWorkingHead(history, kElementId + 1); };
  const auto working  = [&] { return test::EditorWorkingPreview(*pipeline, kElementId + 1); };
  ASSERT_TRUE(std::filesystem::create_directory(paths.journal));
  const auto prior_hash         = CanonicalPipelineDocumentJson(working()->Document());
  const auto prior_head         = head_of();
  const auto prior_commit_count = graph_of()->CommitCount();
  const auto prior_reason       = history.LastPublishedRenderReason();
  const auto prior_counter      = working()->Document().NextColorGradeNameNumber();

  const auto topology           = BuildTopologyEdit(working()->Document());
  error.clear();
  EXPECT_FALSE(history.EditNodeGraph(handle, topology.change, &error));
  EXPECT_EQ(error, "mini-Git journal file could not be opened for append");
  EXPECT_EQ(CanonicalPipelineDocumentJson(working()->Document()), prior_hash);
  EXPECT_EQ(head_of(), prior_head);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  const std::vector<NodeId> expected_initial_ids = {NodeId{"develop"}, NodeId{"grade.primary"},
                                                    NodeId{"drt"}};
  EXPECT_EQ(GraphNodeIds(working()->Document()), expected_initial_ids);
  history.Release(handle);
}

TEST(NodeGraphTopologyHistory, MaskGroupTopInsertAndBridgeRemoveCommitOnceAndReplayThroughUndo) {
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(EditorSessionPipelineMappers{{}, [](sl_element_id_t id) {
                                                       return test::MakeInMemoryEditorLease(
                                                           id, UnprotectedPrimaryGradeDocument());
                                                     }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 2, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto graph_of   = [&] { return test::EditorHistoryGraph(history, kElementId + 2); };
  const auto head_of    = [&] { return test::EditorWorkingHead(history, kElementId + 2); };
  const auto working    = [&] { return test::EditorWorkingPreview(*pipeline, kElementId + 2); };

  const auto prior_hash = CanonicalPipelineDocumentJson(working()->Document());
  const auto prior_head = head_of();
  const auto prior_commit_count = graph_of()->CommitCount();

  // Stale-predecessor rejection: the node before DRT is grade.primary, not DRT.
  EXPECT_FALSE(history.InsertColorGradeAtTop(handle, NodeId{"grade.stale"}, NodeId{"drt"}, &error));
  EXPECT_EQ(error, "The Mask Groups insertion point changed since the request was issued");
  EXPECT_EQ(CanonicalPipelineDocumentJson(working()->Document()), prior_hash);
  EXPECT_EQ(head_of(), prior_head);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(working()->Document().Graph().FindNode(NodeId{"grade.stale"}), nullptr);

  // One typed commit inserts the clean grade between grade.primary and DRT.
  error.clear();
  ASSERT_TRUE(
      history.InsertColorGradeAtTop(handle, NodeId{"grade.top"}, NodeId{"grade.primary"}, &error))
      << error;
  const std::vector<NodeId> inserted_backbone = {NodeId{"develop"}, NodeId{"grade.primary"},
                                                 NodeId{"grade.top"}, NodeId{"drt"}};
  EXPECT_EQ(working()->Document().Graph().ImageBackboneNodeIds(), inserted_backbone);
  EXPECT_EQ(working()->Document().Graph().FindNode(NodeId{"grade.top"})->DisplayName(),
            "Color Grade 2");
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count + 1);
  ASSERT_TRUE(history.LastPublishedRenderReason().has_value());
  EXPECT_EQ(*history.LastPublishedRenderReason(), EditorRenderReason::GraphTopologyChanged);
  const auto inserted_hash = CanonicalPipelineDocumentJson(working()->Document());
  const auto inserted_top_json =
      working()->Document().Graph().FindNode(NodeId{"grade.top"})->ToJson().dump();

  EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(std::any_of(snapshot.commits.begin(), snapshot.commits.end(), [](const auto& commit) {
    return commit.operation_kind == "add_color_grade";
  }));

  // Undo removes the node; Redo restores the exact stored state.
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  EXPECT_EQ(working()->Document().Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(working()->Document().Graph().FindNode(NodeId{"grade.top"}), nullptr);
  ASSERT_TRUE(history.Redo(handle, &error)) << error;
  EXPECT_EQ(CanonicalPipelineDocumentJson(working()->Document()), inserted_hash);

  // Bridge-remove the inserted grade: one commit, predecessor wired to successor.
  error.clear();
  ASSERT_TRUE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.top"}, &error)) << error;
  EXPECT_EQ(working()->Document().Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_NE(alcedo::FindSceneImageEdge(working()->Document().Graph(), NodeId{"develop"},
                                       NodeId{"grade.primary"}),
            nullptr);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count + 2);

  // Removing the last remaining Color Grade leaves Develop connected to DRT.
  error.clear();
  ASSERT_TRUE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.primary"}, &error)) << error;
  EXPECT_EQ(working()->Document().Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"drt"}}));
  EXPECT_NE(
      alcedo::FindSceneImageEdge(working()->Document().Graph(), NodeId{"develop"}, NodeId{"drt"}),
      nullptr);

  // Endpoints are never removable; failures publish no commit.
  const auto commits_before_endpoint = graph_of()->CommitCount();
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"develop"}, &error));
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"drt"}, &error));
  EXPECT_EQ(graph_of()->CommitCount(), commits_before_endpoint);

  // Both removals undo back to the stored post-insert topology. Node and edge
  // container positions are not part of the stored typed change, so the check is
  // semantic: identical backbone order and identical restored node JSON.
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  ASSERT_TRUE(history.Undo(handle, &error)) << error;
  EXPECT_EQ(working()->Document().Graph().ImageBackboneNodeIds(), inserted_backbone);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(working()->Document().Graph().FindNode(NodeId{"grade.top"})->ToJson().dump(),
            inserted_top_json);
  history.Release(handle);
}

TEST(NodeGraphTopologyHistory, MaskGroupJournalFailureLeavesDocumentHeadAndCounterUntouched) {
  TemporaryProject temporary;
  const auto&      paths    = temporary.paths();

  auto             pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(EditorSessionPipelineMappers{{}, [](sl_element_id_t id) {
                                                       return test::MakeInMemoryEditorLease(
                                                           id, UnprotectedPrimaryGradeDocument());
                                                     }});
  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[path = paths.journal](sl_element_id_t) { return path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto  handle = history.Acquire(kElementId + 3, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto graph_of = [&] { return test::EditorHistoryGraph(history, kElementId + 3); };
  const auto head_of  = [&] { return test::EditorWorkingHead(history, kElementId + 3); };
  const auto working  = [&] { return test::EditorWorkingPreview(*pipeline, kElementId + 3); };
  ASSERT_TRUE(std::filesystem::create_directory(paths.journal));
  const auto prior_hash         = CanonicalPipelineDocumentJson(working()->Document());
  const auto prior_head         = head_of();
  const auto prior_commit_count = graph_of()->CommitCount();
  const auto prior_reason       = history.LastPublishedRenderReason();
  const auto prior_counter      = working()->Document().NextColorGradeNameNumber();

  EXPECT_FALSE(
      history.InsertColorGradeAtTop(handle, NodeId{"grade.top"}, NodeId{"grade.primary"}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(CanonicalPipelineDocumentJson(working()->Document()), prior_hash);
  EXPECT_EQ(head_of(), prior_head);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(working()->Document().Graph().FindNode(NodeId{"grade.top"}), nullptr);

  error.clear();
  EXPECT_FALSE(history.RemoveColorGradeAndBridge(handle, NodeId{"grade.primary"}, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(CanonicalPipelineDocumentJson(working()->Document()), prior_hash);
  EXPECT_EQ(head_of(), prior_head);
  EXPECT_EQ(graph_of()->CommitCount(), prior_commit_count);
  EXPECT_EQ(working()->Document().NextColorGradeNameNumber(), prior_counter);
  EXPECT_EQ(history.LastPublishedRenderReason(), prior_reason);
  history.Release(handle);
}

}  // namespace
}  // namespace alcedo::ui
