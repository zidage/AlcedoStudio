//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/parameter_revision.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

auto WhiteBalanceFields() -> DirtyFieldMask { return DirtyFieldMask{DevelopDirty::WhiteBalance}; }

auto SensorFields() -> DirtyFieldMask {
  return DirtyFieldMask{DevelopDirty::Demosaic} | DirtyFieldMask{DevelopDirty::Highlights} |
         DirtyFieldMask{DevelopDirty::Lens};
}

auto PrimaryExposure(PipelineDocument& document) -> ExposureModel& {
  auto* model = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (model == nullptr) {
    throw std::runtime_error("default document has no primary exposure");
  }
  return *model;
}

}  // namespace

TEST(GpuDagModelGraph, ParameterRevisionsFromConcurrentThreadsAreUnique) {
  constexpr std::size_t                       kThreads   = 4;
  constexpr std::size_t                       kPerThread = 5000;
  std::vector<std::vector<ParameterRevision>> taken(kThreads);
  std::vector<std::thread>                    threads;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&taken, thread] {
      taken[thread].reserve(kPerThread);
      for (std::size_t index = 0; index < kPerThread; ++index) {
        taken[thread].push_back(NextParameterRevision());
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::vector<ParameterRevision> all;
  for (const auto& revisions : taken) {
    EXPECT_TRUE(std::is_sorted(revisions.begin(), revisions.end()));
    all.insert(all.end(), revisions.begin(), revisions.end());
  }
  std::sort(all.begin(), all.end());
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
  EXPECT_EQ(std::count(all.begin(), all.end(), kNoParameterRevision), 0);
}

TEST(GpuDagModelGraph, NewModelsTakeDistinctNonZeroRevisions) {
  ExposureModel first;
  ExposureModel second;
  EXPECT_NE(first.Revision(), kNoParameterRevision);
  EXPECT_NE(second.Revision(), kNoParameterRevision);
  EXPECT_NE(first.Revision(), second.Revision());
}

TEST(GpuDagModelGraph, EachChangingWriteTakesANewRevisionAndKeepsTheLatestValue) {
  ExposureModel model;
  const auto    created = model.Revision();
  model.SetValue(0.1f);
  const auto first = model.Revision();
  model.SetValue(0.3f);
  const auto second = model.Revision();

  EXPECT_NE(first, created);
  EXPECT_NE(second, first);
  EXPECT_FLOAT_EQ(model.Value(), 0.3f);
}

TEST(GpuDagModelGraph, WriteOfTheCurrentValueKeepsTheRevision) {
  ExposureModel model;
  model.SetValue(0.75f);
  const auto revision = model.Revision();
  model.SetValue(0.75f);
  EXPECT_EQ(model.Revision(), revision);
}

TEST(GpuDagModelGraph, ReadsDoNotChangeTheRevision) {
  ExposureModel model;
  model.SetValue(0.75f);
  const auto revision = model.Revision();
  OperatorModelFullDtoCopyCount::Reset();
  const auto dto = model.MakeFullDto();
  (void)model.ToJson();
  (void)model.FieldsRevision(DirtyFieldMask{ExposureTraits::Dirty::All});
  const auto* payload = PayloadAs<ScalarFloatPayload>(dto.payload.get());
  ASSERT_NE(payload, nullptr);
  EXPECT_FLOAT_EQ(payload->value, 0.75f);
  EXPECT_EQ(model.Revision(), revision);
}

TEST(GpuDagModelGraph, FieldsRevisionChangesOnlyForTheWrittenFieldGroup) {
  DevelopParamsModel develop;
  const auto         sensor_before        = develop.FieldsRevision(SensorFields());
  const auto         white_balance_before = develop.FieldsRevision(WhiteBalanceFields());

  auto               payload              = develop.Params();
  payload.wb_mode                         = "custom";
  payload.custom_cct                      = payload.custom_cct + 250.0f;
  develop.ReplaceParams(payload);

  EXPECT_EQ(develop.FieldsRevision(SensorFields()), sensor_before);
  EXPECT_NE(develop.FieldsRevision(WhiteBalanceFields()), white_balance_before);
  EXPECT_EQ(develop.Revision(), develop.FieldsRevision(WhiteBalanceFields()));
}

TEST(GpuDagModelGraph, OneWriteOfTwoFieldsStampsBothFields) {
  SharpenModel model;
  model.SetAmount(8.0f);
  const auto amount = model.FieldsRevision(DirtyFieldMask{SharpenDirty::Amount});
  model.SetRadius(3.0f);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{SharpenDirty::Amount}), amount);
  EXPECT_NE(model.FieldsRevision(DirtyFieldMask{SharpenDirty::Radius}), amount);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{}), kNoParameterRevision);
}

TEST(GpuDagModelGraph, CopyRevisionsFromTakesTheSourceStampsAndRejectsAnotherType) {
  ExposureModel source;
  source.SetValue(0.5f);
  ExposureModel copy;
  copy.SetValue(0.5f);
  ASSERT_NE(copy.Revision(), source.Revision());

  copy.CopyRevisionsFrom(source);
  EXPECT_EQ(copy.Revision(), source.Revision());

  ContrastModel other;
  EXPECT_THROW(copy.CopyRevisionsFrom(other), std::invalid_argument);
  EXPECT_EQ(copy.Revision(), source.Revision());
}

TEST(GpuDagModelGraph, GradeMixAndEnabledWritesTakeNewMixRevisions) {
  auto       document = CreateDefaultPipelineDocument();
  auto*      grade    = document.PrimaryGrade();
  const auto initial  = grade->MixRevision();
  grade->SetMix(grade->Mix());
  EXPECT_EQ(grade->MixRevision(), initial);
  grade->SetMix(0.5f);
  const auto mixed = grade->MixRevision();
  EXPECT_NE(mixed, initial);
  grade->SetEnabled(false);
  EXPECT_NE(grade->MixRevision(), mixed);
}

TEST(GpuDagModelGraph, ClonedDocumentKeepsEveryParameterRevisionOfItsSource) {
  auto document = CreateDefaultPipelineDocument();
  PrimaryExposure(document).SetValue(1.25f);
  document.PrimaryGrade()->SetMix(0.75f);

  const auto clone = ClonePipelineDocument(document);

  EXPECT_EQ(clone.ToJson(), document.ToJson());
  EXPECT_EQ(clone.Develop()->Params().Revision(), document.Develop()->Params().Revision());
  EXPECT_EQ(clone.Develop()->Params().FieldsRevision(WhiteBalanceFields()),
            document.Develop()->Params().FieldsRevision(WhiteBalanceFields()));
  EXPECT_EQ(clone.Drt()->Params().Revision(), document.Drt()->Params().Revision());
  ASSERT_EQ(clone.Drt()->AdjustmentCount(), document.Drt()->AdjustmentCount());
  for (std::size_t index = 0; index < clone.Drt()->AdjustmentCount(); ++index) {
    EXPECT_EQ(clone.Drt()->AdjustmentAt(index).Revision(),
              document.Drt()->AdjustmentAt(index).Revision())
        << index;
  }
  EXPECT_EQ(clone.PrimaryGrade()->MixRevision(), document.PrimaryGrade()->MixRevision());
  ASSERT_EQ(clone.PrimaryGrade()->AdjustmentCount(), document.PrimaryGrade()->AdjustmentCount());
  for (std::size_t index = 0; index < clone.PrimaryGrade()->AdjustmentCount(); ++index) {
    EXPECT_EQ(clone.PrimaryGrade()->AdjustmentAt(index).Revision(),
              document.PrimaryGrade()->AdjustmentAt(index).Revision())
        << index;
  }
  EXPECT_EQ(clone.TopologyRevision(), document.TopologyRevision());
}

TEST(GpuDagModelGraph, WriteToAClonedDocumentChangesOnlyTheClone) {
  auto       document = CreateDefaultPipelineDocument();
  auto       clone    = ClonePipelineDocument(document);
  const auto source   = PrimaryExposure(document).Revision();

  PrimaryExposure(clone).SetValue(2.0f);

  EXPECT_EQ(PrimaryExposure(document).Revision(), source);
  EXPECT_NE(PrimaryExposure(clone).Revision(), source);
}

TEST(GpuDagModelGraph, TopologyChangeTakesANewTopologyRevision) {
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = document.TopologyRevision();
  document.MarkTopologyChanged();
  EXPECT_NE(document.TopologyRevision(), before);
}

}  // namespace alcedo
