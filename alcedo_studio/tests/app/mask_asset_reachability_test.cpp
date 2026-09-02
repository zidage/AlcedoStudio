//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/mask_asset_reachability.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
#include "grade_owned_mask_support.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

auto MakeDescriptor() -> MaskAssetDescriptor {
  MaskAssetDescriptor descriptor;
  descriptor.extent           = {2, 2};
  descriptor.reference_bounds = {0.0f, 0.0f, 1.0f, 1.0f};
  return descriptor;
}

auto PutPixels(MaskStore* store, std::uint8_t fill) -> MaskAssetKey {
  const std::vector<std::uint8_t> pixels(4, fill);
  return store->Put(MakeDescriptor(), pixels);
}

auto AddMaskBatchForKey(const MaskAssetKey& key, const std::string& mask_id) -> PipelineEditBatch {
  const auto mask = grade_mask_test::MakeBrushMask(MaskId{mask_id}, key, MakeDescriptor());
  return MakeAddMaskBatch(NodeId{"grade.primary"}, MaskId{mask_id}, MaskModelToJson(mask), 0);
}

auto HexEncodeKeyBytes(std::string_view key) -> std::string {
  constexpr char digits[] = "0123456789abcdef";
  std::string    encoded;
  encoded.reserve(key.size() * 2);
  for (const unsigned char byte : key) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
}

TEST(MaskAssetReachabilityTest, InactiveVersionAndWalKeepReferencedMaskAssets) {
  const auto root =
      std::filesystem::path{"build/tmp/node_history"} / "reachability_keep_referenced";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  MaskStore store(root);

  const auto live_key    = PutPixels(&store, 11);
  const auto version_key = PutPixels(&store, 22);
  const auto wal_key     = PutPixels(&store, 33);
  const auto unused_key  = PutPixels(&store, 44);

  auto live_document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(live_document, MaskId{"mask.live"}, live_key, MakeDescriptor());

  const auto version_batch  = AddMaskBatchForKey(version_key, "mask.inactive");
  const auto version_commit = EditCommit::MakePipelineEdit(Hash128{}, std::nullopt, version_batch);
  const auto wal_batch      = AddMaskBatchForKey(wal_key, "mask.wal");
  const auto wal_commit     = EditCommit::MakePipelineEdit(Hash128{}, std::nullopt, wal_batch);
  MiniGitJournalRecord wal_record;
  wal_record.kind        = MiniGitJournalRecordKind::kEditCommit;
  wal_record.edit_commit = wal_commit;

  MaskAssetReachabilityScan scan;
  scan.documents.push_back(&live_document);
  scan.commits.push_back(&version_commit);
  scan.wal_records.push_back(&wal_record);

  const auto reachable = CollectReachableMaskAssetKeys(scan);
  EXPECT_TRUE(reachable.contains(live_key));
  EXPECT_TRUE(reachable.contains(version_key));
  EXPECT_TRUE(reachable.contains(wal_key));
  EXPECT_FALSE(reachable.contains(unused_key));

  const auto report = DeleteUnreachableMaskAssetFiles(store, reachable);
  EXPECT_EQ(report.removed_paths.size(), 1u);
  EXPECT_TRUE(report.failures.empty());
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(live_key)));
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(version_key)));
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(wal_key)));
  EXPECT_FALSE(std::filesystem::exists(store.PathFor(unused_key)));
}

TEST(MaskAssetReachabilityTest, MaintenanceRemovesOnlyUnreferencedExactMaskAssetFiles) {
  const auto root =
      std::filesystem::path{"build/tmp/node_history"} / "reachability_delete_unreferenced";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  MaskStore store(root);

  const auto keep_key = PutPixels(&store, 51);
  const auto drop_key = PutPixels(&store, 62);

  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.keep"}, keep_key, MakeDescriptor());

  const auto tmp_path = store.Root() / "00.tmp.skip.r8mask";
  {
    std::ofstream tmp(tmp_path, std::ios::binary);
    ASSERT_TRUE(tmp.is_open());
    tmp << "temporary";
  }

  const auto corrupt_key  = MaskAssetKey{"dead"};
  const auto corrupt_path = store.Root() / (HexEncodeKeyBytes(corrupt_key.Value()) + ".r8mask");
  {
    std::ofstream corrupt(corrupt_path, std::ios::binary);
    ASSERT_TRUE(corrupt.is_open());
    corrupt << "not-a-mask-file";
  }

  MaskAssetReachabilityScan scan;
  scan.documents.push_back(&document);
  const auto reachable = CollectReachableMaskAssetKeys(scan);
  EXPECT_TRUE(reachable.contains(keep_key));
  EXPECT_FALSE(reachable.contains(drop_key));

  const auto report = DeleteUnreachableMaskAssetFiles(store, reachable);
  ASSERT_EQ(report.removed_paths.size(), 1u);
  EXPECT_EQ(report.removed_paths.front(), store.PathFor(drop_key));
  ASSERT_FALSE(report.failures.empty());
  EXPECT_NE(report.failures.front().find(corrupt_path.string()), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(keep_key)));
  EXPECT_FALSE(std::filesystem::exists(store.PathFor(drop_key)));
  EXPECT_TRUE(std::filesystem::exists(corrupt_path));
  EXPECT_TRUE(std::filesystem::exists(tmp_path));
}

}  // namespace
}  // namespace alcedo
