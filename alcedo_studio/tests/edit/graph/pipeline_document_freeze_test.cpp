//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Copy-on-write freeze of PipelineDocument (executor ownership refactor, phase P2).
//
// A frozen document shares nodes, adjustment Models, and Mask lists with the working document.
// These tests prove the two halves of that design: every write path of the working document
// leaves an earlier frozen document unchanged, and a write copies only the parts it changes
// (and nothing at all when no frozen document shares them).

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

auto MutableExposure(PipelineDocument& document) -> ExposureModel& {
  auto* model = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (model == nullptr) {
    throw std::logic_error("Primary Grade has no Exposure");
  }
  return *model;
}

auto ExposureEv(const PipelineDocument& document) -> float {
  return document.PrimaryGrade()
      ->FindAdjustmentByType(type_ids::Exposure())
      ->ToJson()
      .at("exposure_ev")
      .get<float>();
}

auto MakeRadialMask(const std::string& id, float center_x) -> MaskModel {
  MaskModel mask;
  mask.id     = MaskId{id};
  mask.source = RadialMaskSource{center_x, 0.5f, 0.2f, 0.1f, 0.0f, 0.05f, 0.1f};
  return mask;
}

/// Default document whose primary Grade holds two Radial Masks.
auto MakeDocumentWithMasks() -> PipelineDocument {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  grade->AddMask(MakeRadialMask("mask.a", 0.25f), 0);
  grade->AddMask(MakeRadialMask("mask.b", 0.75f), 1);
  return document;
}

TEST(PipelineDocumentFreeze, FrozenDocumentKeepsParameterValuesAfterEveryWorkingDocumentWrite) {
  auto       document    = CreateDefaultPipelineDocument();
  const auto frozen      = document.Freeze();
  const auto frozen_json = frozen->ToJson();
  ASSERT_EQ(frozen_json, document.ToJson());

  MutableExposure(document).SetValue(ExposureEv(document) + 1.0f);
  document.Develop()->Params().ApplyColorTemperatureUpdate(
      DevelopColorTemperatureUpdate{.wb_mode = std::string{"custom"}, .custom_cct = 4200.0f});
  document.Drt()->Params().ApplyUpdate(DrtParameterUpdate{.peak_luminance = 1000.0f});
  auto* sharpen =
      dynamic_cast<SharpenModel*>(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  ASSERT_NE(sharpen, nullptr);
  sharpen->SetAmount(0.5f);
  document.PrimaryGrade()->SetMix(0.4f);
  document.PrimaryGrade()->SetEnabled(false);
  document.PrimaryGrade()->SetDisplayName("Renamed");
  document.PrimaryGrade()->SetDeletionProtected(false);
  document.Geometry().SetRotationDegrees(12.0f);
  document.SetNextColorGradeNameNumber(9);

  EXPECT_NE(document.ToJson(), frozen_json);
  EXPECT_EQ(frozen->ToJson(), frozen_json);
}

TEST(PipelineDocumentFreeze, FrozenDocumentKeepsMasksAfterEveryWorkingDocumentMaskWrite) {
  auto        document     = MakeDocumentWithMasks();
  const auto  frozen       = document.Freeze();
  const auto  frozen_json  = frozen->ToJson();
  const auto* frozen_grade = frozen->PrimaryGrade();
  const auto  revision_a   = frozen_grade->MaskContentRevision(MaskId{"mask.a"});
  const auto  revision_b   = frozen_grade->MaskContentRevision(MaskId{"mask.b"});
  auto*       grade        = document.PrimaryGrade();

  grade->SetMaskOpacity(MaskId{"mask.a"}, 0.3f);
  grade->ReplaceMaskSource(MaskId{"mask.b"},
                           RadialMaskSource{0.9f, 0.1f, 0.3f, 0.3f, 0.0f, 0.0f, 0.0f});
  grade->SetMaskInvert(MaskId{"mask.a"}, true);
  grade->SetMaskEnabled(MaskId{"mask.b"}, false);
  grade->SetMaskDeletionProtected(MaskId{"mask.a"}, true);
  grade->MoveMaskForDisplay(MaskId{"mask.a"}, 1);
  grade->AddMask(MakeRadialMask("mask.c", 0.5f), 0);
  grade->RemoveMask(MaskId{"mask.b"});
  grade->FindMask(MaskId{"mask.a"})->display_name = "Edited through FindMask";
  grade->MaskAt(0).opacity                        = 0.1f;

  EXPECT_NE(document.ToJson(), frozen_json);
  EXPECT_EQ(frozen->ToJson(), frozen_json);
  EXPECT_EQ(frozen_grade->MaskContentRevision(MaskId{"mask.a"}), revision_a);
  EXPECT_EQ(frozen_grade->MaskContentRevision(MaskId{"mask.b"}), revision_b);
  EXPECT_EQ(frozen_grade->MaskContentRevision(MaskId{"mask.c"}), 0u);
}

TEST(PipelineDocumentFreeze, FrozenDocumentKeepsTopologyAfterWorkingDocumentGraphCommands) {
  auto       document    = CreateDefaultPipelineDocument();
  const auto frozen      = document.Freeze();
  const auto frozen_json = frozen->ToJson();
  const auto topology    = frozen->TopologyRevision();

  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.added"}).empty());
  ASSERT_TRUE(SetColorGradeMix(document, NodeId{"grade.added"}, 0.5f).empty());
  document.InsertAdjustment(NodeId{"grade.added"}, 0,
                            AdjustmentInstanceId{"grade.added.second_exposure"},
                            std::make_unique<ExposureModel>());
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());

  EXPECT_EQ(frozen->ToJson(), frozen_json);
  EXPECT_EQ(frozen->Graph().NodeCount(), 3u);
  EXPECT_NE(frozen->Graph().FindNode(NodeId{"grade.primary"}), nullptr);
  EXPECT_EQ(frozen->Graph().FindNode(NodeId{"grade.added"}), nullptr);
  EXPECT_EQ(frozen->TopologyRevision(), topology);
  EXPECT_NE(document.TopologyRevision(), topology);
}

TEST(PipelineDocumentFreeze, ParameterEditCopiesOnlyTheEditedNodeAndModel) {
  auto       document = MakeDocumentWithMasks();
  const auto frozen   = document.Freeze();

  MutableExposure(document).SetValue(ExposureEv(document) + 0.5f);

  // Develop and DRT were not written: still the same objects.
  EXPECT_EQ(std::as_const(document).Develop(), frozen->Develop());
  EXPECT_EQ(std::as_const(document).Drt(), frozen->Drt());
  // The primary Grade was copied; inside it only the Exposure Model was copied.
  const auto* grade        = std::as_const(document).PrimaryGrade();
  const auto* frozen_grade = frozen->PrimaryGrade();
  ASSERT_NE(grade, frozen_grade);
  ASSERT_EQ(grade->AdjustmentCount(), frozen_grade->AdjustmentCount());
  std::size_t copied_models = 0;
  for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
    if (&grade->AdjustmentAt(index) != &frozen_grade->AdjustmentAt(index)) {
      ++copied_models;
      EXPECT_EQ(grade->AdjustmentAt(index).Type(), type_ids::Exposure());
    }
  }
  EXPECT_EQ(copied_models, 1u);
  // The Mask list was not written, so its storage is still shared.
  EXPECT_EQ(grade->Masks().data(), frozen_grade->Masks().data());
}

TEST(PipelineDocumentFreeze, WorkingDocumentWritesInPlaceWhenNoFrozenDocumentSharesIt) {
  auto        document = MakeDocumentWithMasks();
  const auto* grade    = std::as_const(document).PrimaryGrade();
  const auto* exposure = &MutableExposure(document);
  const auto* masks    = grade->Masks().data();

  MutableExposure(document).SetValue(0.25f);
  document.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.5f);
  EXPECT_EQ(std::as_const(document).PrimaryGrade(), grade);
  EXPECT_EQ(&MutableExposure(document), exposure);
  EXPECT_EQ(grade->Masks().data(), masks);

  // After the last frozen holder releases its document, writes are in place again.
  {
    const auto frozen = document.Freeze();
    MutableExposure(document).SetValue(0.5f);
  }
  const auto* after_copy = std::as_const(document).PrimaryGrade();
  const auto* copied     = &MutableExposure(document);
  MutableExposure(document).SetValue(0.75f);
  EXPECT_EQ(std::as_const(document).PrimaryGrade(), after_copy);
  EXPECT_EQ(&MutableExposure(document), copied);
  EXPECT_FLOAT_EQ(ExposureEv(document), 0.75f);
}

TEST(PipelineDocumentFreeze, NonConstLookupCopiesOnlyTheReturnedNode) {
  auto       document = CreateDefaultPipelineDocument();
  const auto frozen   = document.Freeze();

  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  EXPECT_NE(static_cast<const ColorGradeNodeModel*>(grade), frozen->PrimaryGrade());
  EXPECT_EQ(std::as_const(document).Develop(), frozen->Develop());
  EXPECT_EQ(std::as_const(document).Drt(), frozen->Drt());
  // A const read never copies.
  const auto frozen_again = document.Freeze();
  EXPECT_EQ(std::as_const(document).PrimaryGrade(), frozen_again->PrimaryGrade());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"missing"}), nullptr);
}

TEST(PipelineDocumentFreeze, CopiedNodesKeepTheChangeStampsOfTheirSource) {
  auto        document     = MakeDocumentWithMasks();
  const auto  frozen       = document.Freeze();
  const auto* frozen_grade = frozen->PrimaryGrade();
  const auto  fingerprint  = DocumentRevisionFingerprint(*frozen);

  // Copy the Grade node and one Model without changing any value.
  ASSERT_NE(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Contrast()), nullptr);
  const auto* grade = std::as_const(document).PrimaryGrade();
  ASSERT_NE(grade, frozen_grade);
  EXPECT_EQ(grade->MixRevision(), frozen_grade->MixRevision());
  for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
    EXPECT_EQ(grade->AdjustmentAt(index).Revision(), frozen_grade->AdjustmentAt(index).Revision());
  }
  EXPECT_EQ(grade->MaskContentRevision(MaskId{"mask.a"}),
            frozen_grade->MaskContentRevision(MaskId{"mask.a"}));
  // Equal stamps: a renderer that applied the frozen document sees no change.
  EXPECT_EQ(DocumentRevisionFingerprint(document), fingerprint);

  MutableExposure(document).SetValue(ExposureEv(document) + 1.0f);
  EXPECT_NE(DocumentRevisionFingerprint(document), fingerprint);
  EXPECT_EQ(DocumentRevisionFingerprint(*frozen), fingerprint);
}

TEST(PipelineDocumentFreeze, RevisionFingerprintChangesWithEveryStampedWrite) {
  auto document       = MakeDocumentWithMasks();
  auto previous       = DocumentRevisionFingerprint(document);
  auto expect_changed = [&](const char* write) {
    const auto current = DocumentRevisionFingerprint(document);
    EXPECT_NE(current, previous) << write;
    previous = current;
  };

  MutableExposure(document).SetValue(ExposureEv(document) + 1.0f);
  expect_changed("adjustment Model");
  document.Develop()->Params().ApplyColorTemperatureUpdate(
      DevelopColorTemperatureUpdate{.custom_cct = 3000.0f});
  expect_changed("Develop");
  document.Drt()->Params().ApplyUpdate(DrtParameterUpdate{.peak_luminance = 600.0f});
  expect_changed("DRT");
  document.PrimaryGrade()->SetMix(0.2f);
  expect_changed("mix");
  document.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.b"}, 0.2f);
  expect_changed("Mask");
  document.MarkTopologyChanged();
  expect_changed("topology");
  EXPECT_EQ(DocumentRevisionFingerprint(document), previous);
}

// The owner thread edits and freezes while a reader thread reads the frozen documents. Each
// frozen document must still equal the JSON recorded when it was frozen. Reads overlap with
// copy-on-write replacement of shared nodes and Models on the owner thread.
TEST(PipelineDocumentFreeze, FrozenDocumentsReadOnAnotherThreadKeepTheirValuesWhileOwnerEdits) {
  struct Published {
    std::shared_ptr<const PipelineDocument> document;
    std::string                             json;
  };
  auto                  document = MakeDocumentWithMasks();
  std::mutex            mutex;
  std::deque<Published> queue;
  std::atomic<bool>     done{false};
  std::atomic<int>      mismatches{0};
  std::atomic<int>      checked{0};

  std::thread reader([&] {
    while (true) {
      const bool            finished = done.load();
      std::deque<Published> batch;
      {
        std::scoped_lock lock(mutex);
        batch.swap(queue);
      }
      for (const auto& item : batch) {
        // Read twice: once right away, once after the owner had time to write again.
        if (item.document->ToJson().dump() != item.json) {
          ++mismatches;
        }
        std::this_thread::yield();
        if (item.document->ToJson().dump() != item.json) {
          ++mismatches;
        }
        ++checked;
      }
      if (batch.empty() && finished) {
        return;
      }
      std::this_thread::yield();
    }
  });

  constexpr int kIterations = 300;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    auto frozen = document.Freeze();
    auto json   = frozen->ToJson().dump();
    {
      std::scoped_lock lock(mutex);
      queue.push_back({std::move(frozen), std::move(json)});
    }
    MutableExposure(document).SetValue(static_cast<float>(iteration) * 0.01f);
    document.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"},
                                            static_cast<float>(iteration % 10) / 10.0f);
    document.Drt()->Params().ApplyUpdate(
        DrtParameterUpdate{.peak_luminance = 100.0f + static_cast<float>(iteration)});
  }
  done = true;
  reader.join();

  EXPECT_EQ(checked.load(), kIterations);
  EXPECT_EQ(mismatches.load(), 0);
}

}  // namespace
}  // namespace alcedo
