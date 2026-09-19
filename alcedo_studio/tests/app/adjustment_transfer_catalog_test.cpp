//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_catalog.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

auto DocumentWithExposureEv(double exposure_ev) -> PipelineDocument {
  auto        document = CreateDefaultPipelineDocument();
  std::string error;
  if (!ApplyEditorParameterPatch(document, test::ColorGradeFieldTarget("exposure"),
                                 {{"exposure_ev", exposure_ev}}, &error)) {
    throw std::runtime_error(error.empty() ? "Failed to patch exposure" : error);
  }
  return document;
}

auto ExposureEv(const PipelineDocument& document) -> double {
  nlohmann::json value;
  std::string    error;
  EXPECT_TRUE(ReadEditorParameterJson(document, test::ColorGradeFieldTarget("exposure"), &value,
                                      &error))
      << error;
  return value.at("exposure_ev").get<double>();
}

auto GradeAt(PipelineDocument& document, const NodeId& id) -> ColorGradeNodeModel* {
  return dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(id));
}

auto ReadWalBytes(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Complete externally visible session state the catalog read must not change:
/// graph fields, active Version identity, history tip and chain fold, live and
/// immutable documents, journal records, the durable WAL file, and redo state.
struct SessionState {
  std::string                   image_state;
  std::string                   active_version_ref;
  version_ref_id_t              active_version_id{};
  std::string                   active_head;
  std::string                   active_chain;
  std::size_t                   commit_count         = 0;
  std::size_t                   version_count        = 0;
  std::string                   live_document;
  std::string                   root_document;
  bool                          live_topology_dirty  = false;
  std::size_t                   journal_records      = 0;
  std::optional<std::uint64_t>  journal_last_sequence;
  std::uint64_t                 journal_next_sequence = 0;
  std::size_t                   redo_count           = 0;
  std::string                   wal_bytes;
};

void ExpectSessionUnchanged(const SessionState& before, const SessionState& after) {
  EXPECT_EQ(after.image_state, before.image_state);
  EXPECT_EQ(after.active_version_ref, before.active_version_ref);
  EXPECT_EQ(after.active_version_id, before.active_version_id);
  EXPECT_EQ(after.active_head, before.active_head);
  EXPECT_EQ(after.active_chain, before.active_chain);
  EXPECT_EQ(after.commit_count, before.commit_count);
  EXPECT_EQ(after.version_count, before.version_count);
  EXPECT_EQ(after.live_document, before.live_document);
  EXPECT_EQ(after.root_document, before.root_document);
  EXPECT_EQ(after.live_topology_dirty, before.live_topology_dirty);
  EXPECT_EQ(after.journal_records, before.journal_records);
  EXPECT_EQ(after.journal_last_sequence, before.journal_last_sequence);
  EXPECT_EQ(after.journal_next_sequence, before.journal_next_sequence);
  EXPECT_EQ(after.redo_count, before.redo_count);
  EXPECT_EQ(after.wal_bytes, before.wal_bytes);
}

auto ItemsOfKind(const AdjustmentTransferNodeDescriptor& node, AdjustmentTransferItemKind kind)
    -> std::vector<const AdjustmentTransferItemDescriptor*> {
  std::vector<const AdjustmentTransferItemDescriptor*> found;
  for (const auto& item : node.items) {
    if (item.kind == kind) {
      found.push_back(&item);
    }
  }
  return found;
}

}  // namespace

class AdjustmentTransferCatalogHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_             = std::filesystem::path{"build"} / "tmp" / "adjustment_transfer_catalog" /
           std::string{info->name()};
    std::filesystem::create_directories(dir_);
    journal_path_ = dir_ / "image.wal";
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);

    journal_ = std::make_shared<MiniGitJournal>(journal_path_);
    graph_   = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(41));
    history_ = std::make_unique<MiniGitWorkingHistory>(graph_, journal_);
  }

  /// Journal + commit + head move first, then the live-document apply — the
  /// same publish order the production edit path uses.
  auto CommitBatch(const PipelineEditBatch& batch) -> MiniGitEditAppendResult {
    const auto result = history_->AppendEdit(batch);
    if (result.committed) {
      std::string error;
      EXPECT_TRUE(ApplyPipelineEditBatch(live_document_, batch,
                                         PipelineEditApplyDirection::Forward, &error))
          << error;
    }
    return result;
  }

  auto CommitExposure(double before, double after) -> MiniGitEditAppendResult {
    return CommitBatch(MakeSetParameterBatch(test::ColorGradeFieldTarget("exposure"),
                                             {{"exposure_ev", before}},
                                             {{"exposure_ev", after}}, true, true,
                                             "Color Grade 1"));
  }

  [[nodiscard]] auto CaptureSession() const -> SessionState {
    const auto& active  = graph_->GetActiveVersionRef();
    const auto  journal = journal_->Snapshot();
    SessionState state;
    state.image_state           = graph_->GetImageEditState().ToJSON().dump();
    state.active_version_ref    = active.ToJSON().dump();
    state.active_version_id     = graph_->GetActiveVersionId();
    state.active_head           = HeadCommitHashToStorage(active.head_commit_hash);
    state.active_chain          = graph_->ChainHashForHead(active.head_commit_hash).ToString();
    state.commit_count          = graph_->CommitCount();
    state.version_count         = graph_->GetAllVersionRefs().size();
    state.live_document         = CanonicalPipelineDocumentJson(live_document_);
    state.root_document         = CanonicalPipelineDocumentJson(root_document_);
    state.live_topology_dirty   = live_document_.TopologyDirty();
    state.journal_records       = journal.records.size();
    state.journal_last_sequence = journal.last_sequence;
    state.journal_next_sequence = journal_->next_sequence();
    state.redo_count            = history_->redo_count();
    state.wal_bytes             = ReadWalBytes(journal_path_);
    return state;
  }

  std::filesystem::path               dir_;
  std::filesystem::path               journal_path_;
  std::shared_ptr<MiniGitJournal>     journal_;
  std::shared_ptr<CommitGraph>        graph_;
  std::unique_ptr<MiniGitWorkingHistory> history_;
  /// Immutable image root; the catalog reads it but must never mutate it.
  PipelineDocument                    root_document_ = CreateDefaultPipelineDocument();
  /// Live document at the active working tip; the catalog never receives it.
  PipelineDocument                    live_document_ = CreateDefaultPipelineDocument();
};

TEST_F(AdjustmentTransferCatalogHistoryTest,
       CatalogReadsInactiveVersionWithoutChangingActiveVersion) {
  ASSERT_TRUE(CommitExposure(1.5, 2.0).committed);
  // Version-column order is created_at ascending; place the new Version after
  // the graph's Default Version regardless of the wall-clock stamp it got.
  const auto created_after_default = graph_->GetActiveVersionRef().created_at + 1;
  const auto inactive_id =
      graph_->CreateVersionRefAtActiveHead("Warm Portrait", created_after_default);
  ASSERT_TRUE(CommitExposure(2.0, -0.5).committed);

  const auto before = CaptureSession();

  const auto versions = AdjustmentTransferCatalogService::ListVersions(*graph_);
  ASSERT_EQ(versions.size(), 2u);
  EXPECT_EQ(versions.at(0).display_name, "Default");
  EXPECT_TRUE(versions.at(0).active);
  EXPECT_EQ(versions.at(0).source_order, 0u);
  EXPECT_EQ(versions.at(1).version_id, inactive_id);
  EXPECT_EQ(versions.at(1).display_name, "Warm Portrait");
  EXPECT_FALSE(versions.at(1).active);
  EXPECT_EQ(versions.at(1).source_order, 1u);
  ExpectSessionUnchanged(before, CaptureSession());

  std::string error;
  const auto read = AdjustmentTransferCatalogService::ReadVersion(*graph_, root_document_,
                                                                  inactive_id, &error);
  ASSERT_TRUE(read.has_value()) << error;
  EXPECT_EQ(read->version.version_id, inactive_id);
  EXPECT_EQ(read->version.display_name, "Warm Portrait");
  EXPECT_FALSE(read->version.active);
  EXPECT_EQ(read->version.source_order, 1u);

  // The replayed document shows the inactive tip (2.0); the live document stays
  // at the active tip (-0.5) and the immutable root stays at its default (1.5).
  // Three distinct values prove independent document storage.
  EXPECT_NEAR(ExposureEv(read->document), 2.0, 1e-5);
  EXPECT_NEAR(ExposureEv(live_document_), -0.5, 1e-5);
  EXPECT_NEAR(ExposureEv(root_document_), 1.5, 1e-5);

  ASSERT_EQ(read->nodes.size(), 2u);
  EXPECT_EQ(read->nodes.at(0).kind, AdjustmentTransferNodeKind::ColorGrade);
  EXPECT_EQ(read->nodes.at(0).node_id, NodeId{"grade.primary"});
  EXPECT_EQ(read->nodes.at(1).kind, AdjustmentTransferNodeKind::DrtPost);

  ExpectSessionUnchanged(before, CaptureSession());
}

TEST_F(AdjustmentTransferCatalogHistoryTest,
       CatalogReplayFailureKeepsSourceSessionUnchanged) {
  ASSERT_TRUE(CommitExposure(1.5, 2.0).committed);
  const auto good_id = graph_->CreateVersionRefAtActiveHead("Good", 10);

  std::string error;
  const auto good_read = AdjustmentTransferCatalogService::ReadVersion(
      *graph_, root_document_, good_id, &error);
  ASSERT_TRUE(good_read.has_value()) << error;
  const auto good_document_json = CanonicalPipelineDocumentJson(good_read->document);

  // A commit whose target node no document contains: it publishes to the graph
  // and WAL but cannot apply during replay.
  const auto broken = history_->AppendEdit(
      MakeSetParameterBatch(test::ColorGradeFieldTarget("exposure", "grade.ghost"),
                            {{"exposure_ev", 2.0}}, {{"exposure_ev", 3.0}}, true, true, "Ghost"));
  ASSERT_TRUE(broken.committed) << broken.error;
  const auto broken_id = graph_->CreateVersionRefAtActiveHead("Broken", 20);

  const auto before = CaptureSession();

  const auto failed = AdjustmentTransferCatalogService::ReadVersion(*graph_, root_document_,
                                                                    broken_id, &error);
  EXPECT_FALSE(failed.has_value());
  EXPECT_FALSE(error.empty());

  error.clear();
  const auto unknown = AdjustmentTransferCatalogService::ReadVersion(
      *graph_, root_document_, version_ref_id_t{}, &error);
  EXPECT_FALSE(unknown.has_value());
  EXPECT_FALSE(error.empty());

  // The caller's prior valid read result is preserved.
  EXPECT_EQ(CanonicalPipelineDocumentJson(good_read->document), good_document_json);
  ExpectSessionUnchanged(before, CaptureSession());
}

TEST(AdjustmentTransferCatalogTest, CatalogOrdersGradesBySourceBackbone) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  ASSERT_TRUE(
      AddCleanColorGrade(document, NodeId{"grade.extra"}, NodeId{"grade.first"}).empty());
  // Backbone is now primary -> first -> extra -> drt. Names sort in the
  // opposite order on purpose: Alpha < Mike < Zulu vs backbone Zulu, Alpha,
  // Mike.
  document.PrimaryGrade()->SetDisplayName("Zulu");
  ASSERT_NE(GradeAt(document, NodeId{"grade.first"}), nullptr);
  GradeAt(document, NodeId{"grade.first"})->SetDisplayName("Alpha");
  ASSERT_NE(GradeAt(document, NodeId{"grade.extra"}), nullptr);
  GradeAt(document, NodeId{"grade.extra"})->SetDisplayName("Mike");

  std::string error;
  const auto nodes =
      AdjustmentTransferCatalogService::BuildNodeDescriptors(document, &error);
  ASSERT_TRUE(nodes.has_value()) << error;
  ASSERT_EQ(nodes->size(), 4u);
  EXPECT_EQ(nodes->at(0).node_id, NodeId{"grade.primary"});
  EXPECT_EQ(nodes->at(0).display_name, "Zulu");
  EXPECT_TRUE(nodes->at(0).is_default_grade);
  EXPECT_EQ(nodes->at(1).node_id, NodeId{"grade.first"});
  EXPECT_EQ(nodes->at(1).display_name, "Alpha");
  EXPECT_FALSE(nodes->at(1).is_default_grade);
  EXPECT_EQ(nodes->at(2).node_id, NodeId{"grade.extra"});
  EXPECT_EQ(nodes->at(2).display_name, "Mike");
  EXPECT_EQ(nodes->at(3).kind, AdjustmentTransferNodeKind::DrtPost);
  EXPECT_EQ(nodes->at(3).display_name, "DRT and Post Processing");
  for (std::size_t index = 0; index < nodes->size(); ++index) {
    EXPECT_EQ(nodes->at(index).source_order, index);
  }
}

TEST(AdjustmentTransferCatalogTest, CatalogUsesAdjustmentInstanceIdentity) {
  auto  document = DocumentWithExposureEv(1.0);
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  auto extra = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  ASSERT_NE(extra, nullptr);
  auto* extra_exposure = dynamic_cast<ExposureModel*>(extra.get());
  ASSERT_NE(extra_exposure, nullptr);
  extra_exposure->SetValue(-2.0f);
  document.InsertAdjustment(grade->Id(), grade->AdjustmentCount(),
                            AdjustmentInstanceId{"grade.primary.exposure.2"}, std::move(extra));

  std::string error;
  const auto nodes =
      AdjustmentTransferCatalogService::BuildNodeDescriptors(document, &error);
  ASSERT_TRUE(nodes.has_value()) << error;
  ASSERT_FALSE(nodes->empty());

  std::vector<const AdjustmentTransferItemDescriptor*> exposures;
  for (const auto* item : ItemsOfKind(nodes->front(), AdjustmentTransferItemKind::Adjustment)) {
    if (item->type == type_ids::Exposure()) {
      exposures.push_back(item);
    }
  }
  // Two Exposure instances share type and label but never identity.
  ASSERT_EQ(exposures.size(), 2u);
  ASSERT_TRUE(exposures.at(0)->adjustment_id.has_value());
  ASSERT_TRUE(exposures.at(1)->adjustment_id.has_value());
  EXPECT_EQ(*exposures.at(0)->adjustment_id, AdjustmentInstanceId{"grade.primary.exposure"});
  EXPECT_EQ(*exposures.at(1)->adjustment_id,
            AdjustmentInstanceId{"grade.primary.exposure.2"});
  EXPECT_EQ(exposures.at(0)->display_name, exposures.at(1)->display_name);
  EXPECT_EQ(exposures.at(0)->display_value, "1.00");
  EXPECT_EQ(exposures.at(1)->display_value, "-2.00");
}

TEST(AdjustmentTransferCatalogTest, CatalogShowsOneMasksItemForAnyMaskCount) {
  const auto masks_items = [](const PipelineDocument& document) {
    std::string error;
    const auto  nodes =
        AdjustmentTransferCatalogService::BuildNodeDescriptors(document, &error);
    EXPECT_TRUE(nodes.has_value()) << error;
    if (!nodes.has_value() || nodes->empty()) {
      return std::vector<AdjustmentTransferItemDescriptor>{};
    }
    std::vector<AdjustmentTransferItemDescriptor> found;
    for (const auto* item :
         ItemsOfKind(nodes->front(), AdjustmentTransferItemKind::Masks)) {
      found.push_back(*item);
    }
    return found;
  };

  const auto zero = masks_items(CreateDefaultPipelineDocument());
  ASSERT_EQ(zero.size(), 1u);
  EXPECT_EQ(zero.front().kind, AdjustmentTransferItemKind::Masks);
  EXPECT_FALSE(zero.front().enabled);

  auto one = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(one, MaskId{"mask.a"});
  const auto single = masks_items(one);
  ASSERT_EQ(single.size(), 1u);
  EXPECT_TRUE(single.front().enabled);
  EXPECT_EQ(single.front().display_value, "1");

  auto many = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(many, MaskId{"mask.a"});
  grade_mask_test::AddLinearGradientMask(many, MaskId{"mask.b"});
  grade_mask_test::AddRadialMask(many, MaskId{"mask.c"});
  const auto several = masks_items(many);
  ASSERT_EQ(several.size(), 1u);
  EXPECT_TRUE(several.front().enabled);
  EXPECT_EQ(several.front().display_value, "3");
}

TEST(AdjustmentTransferCatalogTest, CatalogBuildsDrtPostItemsFromCurrentOwners) {
  auto        document = CreateDefaultPipelineDocument();
  std::string error;
  const auto  nodes =
      AdjustmentTransferCatalogService::BuildNodeDescriptors(document, &error);
  ASSERT_TRUE(nodes.has_value()) << error;
  ASSERT_FALSE(nodes->empty());

  const auto& endpoint = nodes->back();
  EXPECT_EQ(endpoint.kind, AdjustmentTransferNodeKind::DrtPost);
  EXPECT_EQ(endpoint.node_id, NodeId{"drt"});
  EXPECT_EQ(endpoint.display_name, "DRT and Post Processing");
  ASSERT_EQ(endpoint.items.size(), 5u);

  const auto& params = endpoint.items.at(0);
  EXPECT_EQ(params.kind, AdjustmentTransferItemKind::DrtParameters);
  EXPECT_EQ(params.section, AdjustmentTransferItemSection::DisplayTransform);
  EXPECT_FALSE(params.adjustment_id.has_value());
  EXPECT_EQ(params.display_name, "Display Transform");
  EXPECT_EQ(params.display_value, "OpenDRT");

  const std::array<OperatorTypeId, 4> expected_types{
      type_ids::Clarity(), type_ids::Sharpen(), type_ids::Halation(),
      type_ids::FilmGrain()};
  for (std::size_t index = 0; index < expected_types.size(); ++index) {
    const auto& item = endpoint.items.at(index + 1);
    EXPECT_EQ(item.kind, AdjustmentTransferItemKind::Adjustment);
    EXPECT_EQ(item.section, AdjustmentTransferItemSection::Look);
    EXPECT_EQ(item.type, expected_types.at(index));
    ASSERT_TRUE(item.adjustment_id.has_value());
    EXPECT_EQ(*item.adjustment_id,
              MakeAdjustmentInstanceId(NodeId{"drt"}, expected_types.at(index)));
    EXPECT_EQ(item.source_order, index + 1);
  }

  // Color Grade items never carry DRT-owned types.
  for (std::size_t node_index = 0; node_index + 1 < nodes->size(); ++node_index) {
    for (const auto* item : ItemsOfKind(nodes->at(node_index),
                                      AdjustmentTransferItemKind::Adjustment)) {
      EXPECT_EQ(OwnerOfAdjustment(item->type), AdjustmentParameterOwner::ColorGrade);
    }
  }
}

}  // namespace alcedo
