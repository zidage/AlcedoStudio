//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// PipelineGraphSnapshot and PipelineLineageId (executor ownership refactor, phase P2).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_types.hpp"

namespace alcedo {
namespace {

auto FrozenDefaultDocument() -> std::shared_ptr<const PipelineDocument> {
  return CreateDefaultPipelineDocument().Freeze();
}

TEST(PipelineGraphSnapshot, CommittedSnapshotCarriesItsHistoryIdentity) {
  const auto document = FrozenDefaultDocument();
  const auto lineage  = PipelineLineageId::Next();
  const auto head     = Hash128(1, 2);
  const auto chain    = Hash128(3, 4);

  const auto snapshot = PipelineGraphSnapshot::Committed(document, 42, lineage, head, chain);

  EXPECT_TRUE(snapshot->IsCommitted());
  EXPECT_EQ(&snapshot->Document(), document.get());
  EXPECT_EQ(snapshot->SharedDocument(), document);
  EXPECT_EQ(snapshot->ElementId(), 42u);
  EXPECT_EQ(snapshot->Lineage(), lineage);
  ASSERT_TRUE(snapshot->Head().has_value());
  EXPECT_EQ(*snapshot->Head(), head);
  EXPECT_EQ(snapshot->Chain(), chain);
}

TEST(PipelineGraphSnapshot, CommittedSnapshotOfRootOnlyHistoryHasNoHead) {
  const auto snapshot = PipelineGraphSnapshot::Committed(
      FrozenDefaultDocument(), 7, PipelineLineageId::Next(), std::nullopt, Hash128{});
  EXPECT_TRUE(snapshot->IsCommitted());
  EXPECT_FALSE(snapshot->Head().has_value());
}

TEST(PipelineGraphSnapshot, PreviewSnapshotIsNotCommittedAndHasNoHead) {
  const auto chain    = Hash128(3, 4);
  const auto snapshot = PipelineGraphSnapshot::Preview(FrozenDefaultDocument(), 42,
                                                       PipelineLineageId::Next(), chain);
  EXPECT_FALSE(snapshot->IsCommitted());
  EXPECT_FALSE(snapshot->Head().has_value());
  EXPECT_EQ(snapshot->Chain(), chain);
}

TEST(PipelineGraphSnapshot, SnapshotRequiresADocumentAndALineage) {
  EXPECT_THROW((void)PipelineGraphSnapshot::Committed(nullptr, 1, PipelineLineageId::Next(),
                                                      std::nullopt, Hash128{}),
               std::invalid_argument);
  EXPECT_THROW((void)PipelineGraphSnapshot::Preview(FrozenDefaultDocument(), 1,
                                                    PipelineLineageId{}, Hash128{}),
               std::invalid_argument);
}

TEST(PipelineGraphSnapshot, SnapshotDocumentKeepsValuesAfterWorkingDocumentEdits) {
  auto       working  = CreateDefaultPipelineDocument();
  const auto snapshot = PipelineGraphSnapshot::Preview(working.Freeze(), 1,
                                                       PipelineLineageId::Next(), Hash128{});
  const auto json     = snapshot->Document().ToJson();

  working.PrimaryGrade()->SetMix(0.25f);
  working.Geometry().SetRotationDegrees(5.0f);

  EXPECT_EQ(snapshot->Document().ToJson(), json);
  EXPECT_NE(working.ToJson(), json);
}

TEST(PipelineLineageId, DefaultIdIsEmptyAndNextIdsAreUniqueAcrossThreads) {
  EXPECT_TRUE(PipelineLineageId{}.Empty());

  constexpr int                               kThreads   = 4;
  constexpr int                               kPerThread = 1000;
  std::vector<std::vector<PipelineLineageId>> taken(kThreads);
  std::vector<std::thread>                    threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&taken, thread] {
      for (int index = 0; index < kPerThread; ++index) {
        taken[static_cast<std::size_t>(thread)].push_back(PipelineLineageId::Next());
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::vector<PipelineLineageId> all;
  for (const auto& ids : taken) {
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
    all.insert(all.end(), ids.begin(), ids.end());
  }
  std::sort(all.begin(), all.end());
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
  EXPECT_TRUE(std::none_of(all.begin(), all.end(),
                           [](const PipelineLineageId& id) { return id.Empty(); }));
}

}  // namespace
}  // namespace alcedo
