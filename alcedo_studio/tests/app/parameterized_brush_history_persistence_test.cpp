//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "app/document_transfer.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

auto ReadBytes(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

auto RequireBrush(const PipelineDocument& document, const MaskId& mask_id)
    -> const BrushMaskSource& {
  const auto* mask = document.PrimaryGrade()->FindMask(mask_id);
  EXPECT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  EXPECT_NE(brush, nullptr);
  return *brush;
}

}  // namespace

TEST(ParameterizedBrushHistoryPersistence, ParameterizedBrushSurvivesWalRecoveryAndPaste) {
  const auto root = std::filesystem::path{"build/tmp/brush_stroke_history"} / "wal_paste";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const auto journal_path = root / "image.wal";
  const auto cache_path   = root / "mix-cache.r8mask";
  {
    std::ofstream stream(cache_path, std::ios::binary | std::ios::trunc);
    stream << "disposable-cache";
  }
  const auto cache_before = ReadBytes(cache_path);

  auto journal = std::make_shared<MiniGitJournal>(journal_path);
  auto graph   = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(41));
  const CommitGraph recovery_base = *graph;
  MiniGitWorkingHistory history(graph, journal);

  auto document      = CreateDefaultPipelineDocument();
  const auto first   = grade_mask_test::MakePaintStroke("stroke.first", 8.0f, 12.0f, 4.0f);
  auto second        = grade_mask_test::MakePaintStroke("stroke.second", 16.0f, 9.0f, 5.0f);
  auto mask          = grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.brush"}, {first});
  const auto add     = MakeAddMaskBatch(document.PrimaryGrade()->Id(), mask.id, MaskModelToJson(mask),
                                    0);
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, add, PipelineEditApplyDirection::Forward, &error))
      << error;
  ASSERT_TRUE(history.AppendEdit(add).committed);
  const auto append =
      MakeAppendBrushStrokeBatch(document.PrimaryGrade()->Id(), MaskId{"mask.brush"}, second);
  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, append, PipelineEditApplyDirection::Forward, &error))
      << error;
  ASSERT_TRUE(history.AppendEdit(append).committed);
  const auto move = MakeSetBrushTranslationBatch(document.PrimaryGrade()->Id(), MaskId{"mask.brush"},
                                                 {}, {2.0f, -1.0f});
  ASSERT_TRUE(ApplyPipelineEditBatch(document, move, PipelineEditApplyDirection::Forward, &error))
      << error;
  ASSERT_TRUE(history.AppendEdit(move).committed);
  EXPECT_EQ(append.CanonicalJSON().dump().find("stroke.first"), std::string::npos);
  EXPECT_EQ(document.ToJson().dump().find("asset_key"), std::string::npos);

  MiniGitJournal reopened(journal_path);
  ASSERT_TRUE(reopened.Load(&error)) << error;
  ASSERT_EQ(reopened.records().size(), 3u);
  auto recovered_graph = recovery_base;
  ASSERT_TRUE(MiniGitWorkingHistory::Replay(recovered_graph, reopened.records(), &error)) << error;
  const auto head = recovered_graph.GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(head.has_value());
  const auto commits = FirstParentCommitsForHead(recovered_graph, head);
  ASSERT_EQ(commits.size(), 3u);
  std::filesystem::remove(cache_path, ignored);
  EXPECT_FALSE(std::filesystem::exists(cache_path));

  auto replayed = ReplayPipelineDocumentFromRoot(CreateDefaultPipelineDocument(), commits, &error);
  ASSERT_TRUE(replayed.has_value()) << error;
  const auto& restored = RequireBrush(*replayed, MaskId{"mask.brush"});
  ASSERT_EQ(restored.strokes.size(), 2u);
  EXPECT_EQ(restored.strokes[0].id, StrokeId{"stroke.first"});
  EXPECT_EQ(restored.strokes[1].id, StrokeId{"stroke.second"});
  EXPECT_EQ(BrushStrokeSamples(restored.strokes[0])[0].local_x, 8.0f);
  EXPECT_EQ(BrushStrokeSamples(restored.strokes[1])[0].local_x, 16.0f);
  EXPECT_EQ(restored.placement_translation, (Vector2{2.0f, -1.0f}));
  EXPECT_FALSE(restored.asset_key.has_value());
  EXPECT_TRUE(CollectPersistentMaskAssetKeys(*replayed).empty());
  EXPECT_TRUE(VerifyPersistentMaskAssets(*replayed, nullptr, &error)) << error;
  EXPECT_EQ(replayed->ToJson().dump().find("asset_key"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(cache_path));

  const auto package = CaptureDocumentTransfer(*replayed);
  EXPECT_TRUE(package.mask_assets_.empty());
  EXPECT_EQ(package.schema_, kAdjustmentTransferSchema);
  EXPECT_EQ(package.document_format_version_, kPipelineDocumentFormatVersion);
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  auto target             = CreateDefaultPipelineDocument();
  const auto prepared     = PrepareDocumentPaste(package, target, options);
  EXPECT_TRUE(prepared.package.mask_assets_.empty());
  auto working = ClonePipelineDocument(target);
  ASSERT_TRUE(ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward,
                                     &error))
      << error;
  const auto* pasted_node = working.Graph().FindNode(NodeId{"grade.t1"});
  ASSERT_NE(pasted_node, nullptr);
  const auto* pasted_grade = dynamic_cast<const ColorGradeNodeModel*>(pasted_node);
  ASSERT_NE(pasted_grade, nullptr);
  const auto* pasted_mask = pasted_grade->FindMask(MaskId{"mask.t1"});
  ASSERT_NE(pasted_mask, nullptr);
  const auto* pasted_brush = std::get_if<BrushMaskSource>(&pasted_mask->source);
  ASSERT_NE(pasted_brush, nullptr);
  ASSERT_EQ(pasted_brush->strokes.size(), 2u);
  EXPECT_EQ(pasted_brush->strokes[0].id, StrokeId{"stroke.t1"});
  EXPECT_EQ(pasted_brush->strokes[1].id, StrokeId{"stroke.t2"});
  EXPECT_EQ(BrushStrokeSamples(pasted_brush->strokes[0])[0].local_x, 8.0f);
  EXPECT_EQ(BrushStrokeSamples(pasted_brush->strokes[1])[0].local_x, 16.0f);
  EXPECT_EQ(pasted_brush->placement_translation, (Vector2{2.0f, -1.0f}));
  EXPECT_FALSE(pasted_brush->asset_key.has_value());
  EXPECT_EQ(cache_before, "disposable-cache");
}

}  // namespace alcedo
