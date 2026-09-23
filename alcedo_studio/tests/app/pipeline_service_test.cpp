//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <format>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/pipeline_document_history.hpp"
#include "app/project_service.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {

class PipelineMapperTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    db_path_ = std::filesystem::temp_directory_path() / "sleeve_service_test.db";
    meta_path_ = std::filesystem::temp_directory_path() / "sleeve_service_test.json";
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
  }

  void TearDown() override {
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
  }
};

TEST_F(PipelineMapperTests, InitTest) {
  ProjectService project(db_path_, meta_path_);
  EXPECT_NO_THROW(PipelineMgmtService pipeline_service(project.GetStorage()));
}

TEST_F(PipelineMapperTests, PipelineMgmtServiceBuildsDefaultGpuDagForNewImage) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());
  auto                guard = pipeline_service.LoadPipeline(9001);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->document_, nullptr);
  EXPECT_EQ(guard->document_->Graph().Nodes().size(), 3U);
  EXPECT_EQ(guard->document_->Graph().Edges().size(), 2U);
  EXPECT_EQ(guard->document_->ToJson().at("format_version"), kPipelineDocumentFormatVersion);

  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);
  const auto stored = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(9001);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->at("format_version"), kPipelineDocumentFormatVersion);
  EXPECT_EQ(stored->at("nodes").size(), 3U);
  EXPECT_FALSE(stored->contains("stages"));
}

TEST_F(PipelineMapperTests, BasicPipelineRWTest) {
  std::string pipeline_param;
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    // Load a pipeline that does not exist yet, should get a new pipeline
    auto                pipeline_guard = pipeline_service.LoadPipeline(1);

    EXPECT_NE(pipeline_guard, nullptr);
    EXPECT_EQ(pipeline_guard->id_, 1);
    EXPECT_EQ(pipeline_guard->pinned_, true);
    EXPECT_EQ(pipeline_guard->dirty_, false);

    // Modify the authoritative document.
    auto* exposure = pipeline_guard->document_->PrimaryGrade()->FindAdjustmentByType(
        type_ids::Exposure());
    ASSERT_NE(exposure, nullptr);
    exposure->LoadJson({{"exposure_ev", 2.25f}});
    pipeline_guard->dirty_ = true;

    // Save it back
    pipeline_service.SavePipeline(pipeline_guard);

    // Sync is idempotent after the explicit save and must not change the document.
    pipeline_service.Sync();

    // Load it again and serialize the pipeline to compare
    auto pipeline_guard_2 = pipeline_service.LoadPipeline(1);
    EXPECT_NE(pipeline_guard_2, nullptr);
    EXPECT_EQ(pipeline_guard_2->id_, 1);
    EXPECT_EQ(pipeline_guard_2->pinned_, true);
    EXPECT_EQ(pipeline_guard_2->dirty_,
              false);  // We have sync the cache, so it should not be dirty
    // Serialize the document, not the executor's compatibility stages.
    pipeline_param = pipeline_guard_2->document_->ToJson().dump(2);
  }
  // Leave the scope, reopen and load again
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    auto                pipeline_guard = pipeline_service.LoadPipeline(1);
    EXPECT_NE(pipeline_guard, nullptr);
    EXPECT_EQ(pipeline_guard->id_, 1);
    EXPECT_EQ(pipeline_guard->pinned_, true);
    EXPECT_EQ(pipeline_guard->dirty_, false);  // Not dirty since we just loaded it
    // Serialize the document.
    auto pipeline_param_2 = pipeline_guard->document_->ToJson().dump(2);
    EXPECT_EQ(pipeline_param, pipeline_param_2);
  }
}

TEST_F(PipelineMapperTests, DefaultOutputTransformUsesOpenDRT) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                pipeline_guard = pipeline_service.LoadPipeline(42);
  ASSERT_NE(pipeline_guard, nullptr);
  ASSERT_NE(pipeline_guard->document_->Drt(), nullptr);

  const auto& drt = pipeline_guard->document_->Drt()->Params();
  EXPECT_EQ(drt.Method(), DrtMethod::OpenDrt);
  EXPECT_EQ(drt.EncodingEotf(), DrtEotf::Gamma22);
  EXPECT_EQ(drt.LimitingSpace(), DrtColorSpace::Rec709);
  const auto exported = drt.ToJson();

  pipeline_guard->dirty_ = true;
  pipeline_service.SavePipeline(pipeline_guard);
  pipeline_service.Sync();

  auto reloaded = pipeline_service.LoadPipeline(42);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(exported.dump(), reloaded->document_->Drt()->Params().ToJson().dump());
}

TEST_F(PipelineMapperTests, OutputTransformPersistencePreservesSharedAndMethodSpecificSettings) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                pipeline_guard = pipeline_service.LoadPipeline(43);
  ASSERT_NE(pipeline_guard, nullptr);
  {
    std::unique_lock<std::mutex> render_lock(pipeline_guard->pipeline_->GetRenderLock());
    DrtParameterUpdate           update;
    update.method                 = DrtMethod::Aces20;
    update.encoding_space         = DrtColorSpace::Rec2020;
    update.encoding_eotf          = DrtEotf::St2084;
    update.peak_luminance         = 600.0f;
    update.limiting_space         = DrtColorSpace::P3D65;
    update.look_preset            = "umbra";
    update.tonescale_preset       = "aces_2_0";
    update.creative_white         = "d60";
    update.creative_white_limit   = 23.5f;
    update.display_grey_luminance = 12.5f;
    pipeline_guard->document_->Drt()->Params().ApplyUpdate(std::move(update));
  }

  pipeline_guard->dirty_ = true;
  pipeline_service.SavePipeline(pipeline_guard);
  pipeline_service.Sync();
  project.GetStorage()->ForgetLivePipeline(43);

  PipelineMgmtService reopened(project.GetStorage());
  auto                reloaded = reopened.LoadPipeline(43);
  ASSERT_NE(reloaded, nullptr);
  const auto& drt = reloaded->document_->Drt()->Params();
  EXPECT_EQ(drt.Method(), DrtMethod::Aces20);
  EXPECT_EQ(drt.EncodingSpace(), DrtColorSpace::Rec2020);
  EXPECT_EQ(drt.EncodingEotf(), DrtEotf::St2084);
  EXPECT_FLOAT_EQ(drt.PeakLuminance(), 600.0f);
  EXPECT_EQ(drt.LimitingSpace(), DrtColorSpace::P3D65);
  EXPECT_EQ(drt.LookPreset(), "umbra");
  EXPECT_EQ(drt.TonescalePreset(), "aces_2_0");
  EXPECT_EQ(drt.CreativeWhite(), "d60");
  EXPECT_FLOAT_EQ(drt.CreativeWhiteLimit(), 23.5f);
  EXPECT_FLOAT_EQ(drt.DisplayGreyLuminance(), 12.5f);
  reopened.SavePipeline(reloaded);
}

TEST_F(PipelineMapperTests, SharedGuardPinsUntilLastSave) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                guard_a = pipeline_service.LoadPipeline(7);
  auto                guard_b = pipeline_service.LoadPipeline(7);

  ASSERT_NE(guard_a, nullptr);
  ASSERT_NE(guard_b, nullptr);
  EXPECT_EQ(guard_a.get(), guard_b.get());
  EXPECT_TRUE(guard_a->pinned_);
  EXPECT_EQ(guard_a->pin_count_, 2u);

  pipeline_service.SavePipeline(guard_a);
  EXPECT_TRUE(guard_b->pinned_);
  EXPECT_EQ(guard_b->pin_count_, 1u);

  pipeline_service.SavePipeline(guard_b);
  EXPECT_FALSE(guard_b->pinned_);
  EXPECT_EQ(guard_b->pin_count_, 0u);

  auto guard_c = pipeline_service.LoadPipeline(7);
  ASSERT_NE(guard_c, nullptr);
  EXPECT_TRUE(guard_c->pinned_);
  EXPECT_EQ(guard_c->pin_count_, 1u);
}

TEST_F(PipelineMapperTests, MultiplePipelineTest) {
  constexpr int                           pipeline_count = 5;
  std::array<std::string, pipeline_count> pipeline_params;
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    // Create and save multiple pipelines
    for (sl_element_id_t i = 1; i <= pipeline_count; ++i) {
      auto pipeline_guard = pipeline_service.LoadPipeline(i);
      EXPECT_NE(pipeline_guard, nullptr);
      EXPECT_EQ(pipeline_guard->id_, i);

      // Modify the authoritative document.
      auto* contrast = pipeline_guard->document_->PrimaryGrade()->FindAdjustmentByType(
          type_ids::Contrast());
      ASSERT_NE(contrast, nullptr);
      contrast->LoadJson({{"contrast", static_cast<float>(i) * 0.5f}});
      pipeline_guard->dirty_ = true;

      // Save it back
      pipeline_service.SavePipeline(pipeline_guard);
      pipeline_params[i - 1] = pipeline_guard->document_->ToJson().dump(2);
    }
    // Sync to DB
    pipeline_service.Sync();
  }

  // Reopen and load again to verify
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    for (sl_element_id_t i = 1; i <= pipeline_count; ++i) {
      auto pipeline_guard = pipeline_service.LoadPipeline(i);
      EXPECT_NE(pipeline_guard, nullptr);
      EXPECT_EQ(pipeline_guard->id_, i);

      // Serialize the document.
      auto pipeline_param_2 = pipeline_guard->document_->ToJson().dump(2);
      EXPECT_EQ(pipeline_params[i - 1], pipeline_param_2);
    }
  }
}

TEST_F(PipelineMapperTests, CacheTest1) {
  {
    ProjectService                              project(db_path_, meta_path_);
    PipelineMgmtService                         pipeline_service(project.GetStorage());

    // The default cache size is 64, so we will create 65 pipelines to exceed the cache size
    constexpr int                               pipeline_count = 65;
    std::array<sl_element_id_t, pipeline_count> pipeline_ids;
    for (sl_element_id_t i = 1; i <= pipeline_count; ++i) {
      auto pipeline_guard = pipeline_service.LoadPipeline(i);
      EXPECT_NE(pipeline_guard, nullptr);
      EXPECT_EQ(pipeline_guard->id_, i);
      pipeline_ids[i - 1] = i;

      // Modify the document
      pipeline_guard->document_->PrimaryGrade()
          ->FindAdjustmentByType(type_ids::Exposure())
          ->LoadJson({{"exposure_ev", static_cast<float>(i) * 0.3f}});
      pipeline_guard->dirty_ = true;
      // Save it back
      // So no guard will be pinned
      pipeline_service.SavePipeline(pipeline_guard);
    }
    // Now try to access the first pipeline again, it should be evicted and synced to DB, so it is
    // not dirty
    auto first_pipeline_guard = pipeline_service.LoadPipeline(pipeline_ids[0]);
    EXPECT_NE(first_pipeline_guard, nullptr);
    EXPECT_EQ(first_pipeline_guard->id_, pipeline_ids[0]);
    EXPECT_EQ(first_pipeline_guard->dirty_, false);
  }
}

TEST_F(PipelineMapperTests, CacheTest2) {
  {
    ProjectService                              project(db_path_, meta_path_);
    PipelineMgmtService                         pipeline_service(project.GetStorage());

    // The default cache size is 64, so we will create 70 pipelines to exceed the cache size
    constexpr int                               pipeline_count = 70;
    std::array<sl_element_id_t, pipeline_count> pipeline_ids;
    for (sl_element_id_t i = 0; i < pipeline_count; ++i) {
      auto pipeline_guard = pipeline_service.LoadPipeline(i);
      EXPECT_NE(pipeline_guard, nullptr);
      EXPECT_EQ(pipeline_guard->id_, i);
      pipeline_ids[i] = i;

      // Modify the document
      pipeline_guard->document_->PrimaryGrade()
          ->FindAdjustmentByType(type_ids::Contrast())
          ->LoadJson({{"contrast", static_cast<float>(i) * 0.4f}});
      pipeline_guard->dirty_ = true;

      // No save back, so all pipelines are in use
    }
    // Now try to access the first pipeline again, it should still be in the cache and dirty
    auto first_pipeline_guard = pipeline_service.LoadPipeline(pipeline_ids[0]);
    EXPECT_NE(first_pipeline_guard, nullptr);
    EXPECT_EQ(first_pipeline_guard->id_, pipeline_ids[0]);
    EXPECT_EQ(first_pipeline_guard->dirty_, true);
  }
}

TEST_F(PipelineMapperTests, DISABLED_FuzzTest) {
  {
    ProjectService                                   project(db_path_, meta_path_);
    PipelineMgmtService                              pipeline_service(project.GetStorage());

    constexpr int                                    kOpsCount = 500;
    constexpr int                                    kIdRange  = 96;
    std::mt19937                                     rng{12345};
    std::uniform_int_distribution<int>               id_dist(1, kIdRange);
    std::uniform_int_distribution<int>               op_dist(0, 5);
    std::uniform_real_distribution<float>            value_dist(-2.0f, 2.0f);
    std::unordered_map<sl_element_id_t, std::string> expected_dump;
    const auto empty_dump = CreateDefaultPipelineDocument().ToJson().dump();

    for (int i = 0; i < kOpsCount; ++i) {
      const auto id = static_cast<sl_element_id_t>(id_dist(rng));
      const auto op = op_dist(rng);

      if (op == 0) {
        // Load pipeline (cache hit/miss paths)
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        EXPECT_EQ(guard->id_, id);
        auto dump = guard->document_->ToJson().dump();
        if (expected_dump.contains(id)) {
          EXPECT_EQ(dump, expected_dump.at(id));
        } else {
          // If we never wrote an ID-bound param, it should still be empty
          EXPECT_EQ(dump, empty_dump);
        }
      } else if (op == 1) {
        // Load + modify + save (dirty path)
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        guard->document_->PrimaryGrade()
            ->FindAdjustmentByType(type_ids::Exposure())
            ->LoadJson({{"exposure_ev", static_cast<float>(id) + value_dist(rng)}});
        guard->dirty_ = true;
        pipeline_service.SavePipeline(guard);
        expected_dump[id] = guard->document_->ToJson().dump();
      } else if (op == 2) {
        // Load + modify without save (pinned & dirty in cache)
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        guard->document_->PrimaryGrade()
            ->FindAdjustmentByType(type_ids::Contrast())
            ->LoadJson({{"contrast", static_cast<float>(id) + value_dist(rng)}});
        guard->dirty_     = true;
        expected_dump[id] = guard->document_->ToJson().dump();
      } else if (op == 3) {
        // Sync all dirty pipelines
        pipeline_service.Sync();
      } else if (op == 4) {
        // Stress eviction by accessing a far ID
        auto guard = pipeline_service.LoadPipeline(static_cast<sl_element_id_t>(kIdRange + id));
        ASSERT_NE(guard, nullptr);
        EXPECT_EQ(guard->id_, static_cast<sl_element_id_t>(kIdRange + id));
        auto       dump   = guard->document_->ToJson().dump();
        const auto far_id = static_cast<sl_element_id_t>(kIdRange + id);
        if (expected_dump.contains(far_id)) {
          EXPECT_EQ(dump, expected_dump.at(far_id));
        } else {
          EXPECT_EQ(dump, empty_dump);
        }
      } else {
        // Random read/serialize path
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        auto serialized = guard->document_->ToJson().dump();
        if (expected_dump.contains(id)) {
          EXPECT_EQ(serialized, expected_dump.at(id));
        } else {
          EXPECT_EQ(serialized, empty_dump);
        }
      }
    }

    pipeline_service.Sync();
  }

  // Reopen to verify some pipelines persisted and can be read
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    for (sl_element_id_t id = 1; id <= 10; ++id) {
      auto guard = pipeline_service.LoadPipeline(id);
      ASSERT_NE(guard, nullptr);
      EXPECT_EQ(guard->id_, id);
      auto serialized = guard->document_->ToJson().dump();
      EXPECT_FALSE(serialized.empty());
    }
  }
}

TEST_F(PipelineMapperTests, DISABLED_ThreadSafeTest) {
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  constexpr int            kThreads   = 8;
  constexpr int            kOpsPerThr = 200;
  constexpr int            kIdRange   = 64;

  std::atomic<int>         ops_count{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([t, &pipeline_service, &ops_count]() {
      for (int i = 0; i < kOpsPerThr; ++i) {
        const auto id    = static_cast<sl_element_id_t>((t * kOpsPerThr + i) % kIdRange + 1);
        auto       guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        guard->document_->PrimaryGrade()
            ->FindAdjustmentByType(type_ids::Exposure())
            ->LoadJson({{"exposure_ev", static_cast<float>(id) + static_cast<float>(t) * 0.01f}});
        guard->dirty_ = true;
        pipeline_service.SavePipeline(guard);
        if (i % 10 == 0) {
          pipeline_service.Sync();
        }
        ++ops_count;
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  pipeline_service.Sync();
  EXPECT_EQ(ops_count.load(), kThreads * kOpsPerThr);

  const auto empty_dump = CreateDefaultPipelineDocument().ToJson().dump();
  for (sl_element_id_t id = 1; id <= 10; ++id) {
    auto guard = pipeline_service.LoadPipeline(id);
    ASSERT_NE(guard, nullptr);
    auto serialized = guard->document_->ToJson().dump();
    EXPECT_NE(serialized, empty_dump);
  }
}

TEST_F(PipelineMapperTests, DocumentSaveReloadPreservesNodesEdgesAndParameters) {
  constexpr sl_element_id_t element_id = 8501;
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  auto guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->document_, nullptr);
  {
    std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
    ASSERT_TRUE(AddCleanColorGrade(*guard->document_, NodeId{"drt"}, NodeId{"grade.extra"})
                    .empty());
    ASSERT_TRUE(ReconnectColorGrade(*guard->document_, NodeId{"grade.primary"},
                                    NodeId{"grade.extra"}, NodeId{"drt"})
                    .empty());

    auto* extra = dynamic_cast<ColorGradeNodeModel*>(
        guard->document_->Graph().FindNode(NodeId{"grade.extra"}));
    ASSERT_NE(extra, nullptr);
    ASSERT_TRUE(RenameColorGrade(*guard->document_, NodeId{"grade.extra"}, "Document Look")
                    .empty());
    ASSERT_TRUE(SetColorGradeEnabled(*guard->document_, NodeId{"grade.extra"}, false).empty());
    extra->SetMix(0.625f);
    auto* contrast = extra->FindAdjustmentByType(type_ids::Contrast());
    ASSERT_NE(contrast, nullptr);
    contrast->LoadJson({{"contrast", 12.5f}});
    auto* clarity = dynamic_cast<ClarityModel*>(
        guard->document_->Drt()->FindAdjustmentByType(type_ids::Clarity()));
    auto* sharpen = dynamic_cast<SharpenModel*>(
        guard->document_->Drt()->FindAdjustmentByType(type_ids::Sharpen()));
    ASSERT_NE(clarity, nullptr);
    ASSERT_NE(sharpen, nullptr);
    clarity->SetValue(25.0f);
    sharpen->SetAmount(12.0f);

    ASSERT_TRUE(RemoveColorGradeAndBridge(*guard->document_, NodeId{"grade.primary"}).empty());
  }
  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);

  const auto stored = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_FALSE(stored->contains("stages"));
  EXPECT_FALSE(stored->contains("legacy_stage_adapter"));

  // Force the next service instance through the persisted document boundary.
  project.GetStorage()->ForgetLivePipeline(element_id);
  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadPipeline(element_id);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_, nullptr);
  EXPECT_EQ(loaded->document_->Graph().NodeCount(), 3U);
  EXPECT_EQ(loaded->document_->Graph().Edges().size(), 2U);
  EXPECT_EQ(loaded->document_->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.extra"}, NodeId{"drt"}}));

  const auto* extra = dynamic_cast<const ColorGradeNodeModel*>(
      loaded->document_->Graph().FindNode(NodeId{"grade.extra"}));
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(extra->DisplayName(), "Document Look");
  EXPECT_FALSE(extra->Enabled());
  EXPECT_FLOAT_EQ(extra->Mix(), 0.625f);
  const auto* contrast = extra->FindAdjustmentByType(type_ids::Contrast());
  ASSERT_NE(contrast, nullptr);
  EXPECT_FLOAT_EQ(contrast->ToJson().at("contrast").get<float>(), 12.5f);
  EXPECT_EQ(extra->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  const auto* clarity = dynamic_cast<const ClarityModel*>(
      loaded->document_->Drt()->FindAdjustmentByType(type_ids::Clarity()));
  const auto* sharpen = dynamic_cast<const SharpenModel*>(
      loaded->document_->Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  ASSERT_NE(clarity, nullptr);
  ASSERT_NE(sharpen, nullptr);
  EXPECT_FLOAT_EQ(clarity->Value(), 25.0f);
  EXPECT_FLOAT_EQ(sharpen->Amount(), 12.0f);
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, SavedDocumentContainsNoStageAdapter) {
  constexpr sl_element_id_t element_id = 8502;
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  auto guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  {
    std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
    ASSERT_TRUE(RenameColorGrade(*guard->document_, NodeId{"grade.primary"}, "Saved Grade")
                    .empty());
  }
  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);

  const auto stored = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_FALSE(stored->contains("stages"));
  EXPECT_FALSE(stored->contains("legacy_stage_adapter"));
  ASSERT_TRUE(stored->contains("nodes"));
  const auto stored_grade = std::find_if(
      stored->at("nodes").begin(), stored->at("nodes").end(), [](const nlohmann::json& node) {
        return node.value("id", std::string{}) == "grade.primary";
      });
  ASSERT_NE(stored_grade, stored->at("nodes").end());
  EXPECT_EQ(stored_grade->value("display_name", std::string{}), "Saved Grade");
  project.GetStorage()->ForgetLivePipeline(element_id);

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadPipeline(element_id);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->document_->PrimaryGrade()->DisplayName(), "Saved Grade");
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, InvalidStoredDocumentFailsWithoutReplacement) {
  ProjectService      project(db_path_, meta_path_);
  const auto           storage = project.GetStorage();
  const auto           valid   = CreateDefaultPipelineDocument().ToJson();

  const auto expect_failure = [&](sl_element_id_t element_id, nlohmann::json invalid,
                                  std::string_view expected_text) {
    storage->GetElementStore().UpdatePipelineJsonByElementId(element_id, valid);
    storage->GetElementStore().UpdatePipelineJsonByElementId(element_id, invalid);
    storage->ForgetLivePipeline(element_id);

    PipelineMgmtService loader(storage);
    bool                threw = false;
    std::string         message;
    try {
      (void)loader.LoadPipeline(element_id);
    } catch (const std::exception& error) {
      threw   = true;
      message = error.what();
    }
    EXPECT_TRUE(threw);
    EXPECT_NE(message.find(expected_text), std::string::npos) << message;
    EXPECT_EQ(storage->GetLivePipeline(element_id), nullptr);
  };

  auto missing_nodes = valid;
  missing_nodes.erase("nodes");
  expect_failure(8503, std::move(missing_nodes), "nodes");

  auto invalid_topology = valid;
  invalid_topology["edges"] = nlohmann::json::array();
  expect_failure(8504, std::move(invalid_topology), "graph");

  auto corrupt_params = valid;
  corrupt_params["nodes"][0]["params"] = "corrupt";
  expect_failure(8505, std::move(corrupt_params), "params");

  auto wrong_owner = valid;
  for (auto& node : wrong_owner["nodes"]) {
    if (node.at("id") != "grade.primary") {
      continue;
    }
    node["adjustments"].push_back({{"id", "grade.primary.clarity"},
                                   {"type", std::string{type_ids::Clarity().Text()}},
                                   {"params", {{"clarity", 10.0f}}}});
  }
  expect_failure(8520, std::move(wrong_owner), "belongs to DRT/Post");
}

TEST_F(PipelineMapperTests, FailedDocumentSaveKeepsDirtyStateAndJournal) {
  constexpr sl_element_id_t element_id = 8506;
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  auto guard = pipeline_service.LoadEditorPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);
  const auto stored_before =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored_before.has_value());

  guard = pipeline_service.LoadEditorPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  const auto head_before = guard->working_head_commit_hash();
  guard->serialized_state_needs_writeback_ = true;
  {
    std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
    guard->document_->Graph().Disconnect(NodeId{"develop"}, PortId{"image"},
                                         NodeId{"grade.primary"}, PortId{"image"});
  }
  guard->dirty_ = true;

  EXPECT_THROW(pipeline_service.SavePipeline(guard), std::runtime_error);
  EXPECT_TRUE(guard->dirty_);
  EXPECT_TRUE(guard->serialized_state_needs_writeback_);
  EXPECT_EQ(guard->working_head_commit_hash(), head_before);
  EXPECT_EQ(project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id),
            stored_before);
  EXPECT_EQ(guard->pin_count_, 0U);
}

TEST_F(PipelineMapperTests, SaveDoesNotPersistUnsettledPreviewAsCommittedState) {
  constexpr sl_element_id_t element_id = 8507;
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  auto guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);
  const auto stored_before =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored_before.has_value());

  guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  {
    std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
    ASSERT_TRUE(RenameColorGrade(*guard->document_, NodeId{"grade.primary"}, "Preview Only")
                    .empty());
    guard->unsettled_preview_ = true;
  }
  guard->dirty_ = true;

  EXPECT_THROW(pipeline_service.SavePipeline(guard), std::runtime_error);
  EXPECT_TRUE(guard->dirty_);
  EXPECT_EQ(project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id),
            stored_before);
  EXPECT_EQ(guard->pin_count_, 0U);
}

TEST_F(PipelineMapperTests, EditorLoadUsesMatchingSerializedStateWithoutReconstruction) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto                initial = first.LoadEditorPipeline(701);
  ASSERT_NE(initial, nullptr);
  ASSERT_NE(initial->pipeline_, nullptr);
  EXPECT_NE(initial->root_id_, Hash128{});
  EXPECT_FALSE(initial->working_head_commit_hash().has_value());
  EXPECT_EQ(initial->transaction_chain_hash(), ComputeRootChainHash(initial->root_id_));
  EXPECT_FALSE(initial->serialized_state_needs_writeback_);
  const auto expected_document = initial->document_->ToJson();
  first.SavePipeline(initial);

  // A new service instance forces the editor path to read the serialized state rather than
  // reusing the first service's cache entry.
  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadEditorPipeline(701);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->root_id_, initial->root_id_);
  EXPECT_EQ(loaded->working_head_commit_hash(), std::nullopt);
  EXPECT_EQ(loaded->transaction_chain_hash(), ComputeRootChainHash(initial->root_id_));
  EXPECT_FALSE(loaded->serialized_state_needs_writeback_);
  EXPECT_EQ(loaded->document_->ToJson(), expected_document);
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, ReopenWithMatchingCheckpointSkipsReplay) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto                initial = first.LoadEditorPipeline(731);
  ASSERT_NE(initial, nullptr);
  first.SavePipeline(initial);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       stored = graph_service.LoadGraph(731);
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
    EXPECT_TRUE(IsPipelineDocumentCheckpointJson(
        *stored->GetImageEditState().serialized_pipeline_state));
    EXPECT_FALSE(stored->GetImageEditState().serialized_pipeline_state->contains("pipeline_params"));
  }

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto loaded = reopened.LoadEditorPipeline(731);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u)
      << "matching checkpoint identity must import serialized state without history rebuild";
  EXPECT_FALSE(loaded->serialized_state_needs_writeback_);
  {
    std::unique_lock<std::mutex> render_lock(loaded->pipeline_->GetRenderLock());
    EXPECT_EQ(loaded->pipeline_->GpuDagDocument(), loaded->document_);
  }
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, PersistEditorHistoryStateWritesNewActiveVersionBeforeEditorReopen) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                guard = pipeline_service.LoadEditorPipeline(715);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  const auto expected_materialized_state = guard->commit_graph_->GetImageEditState();

  const auto new_version = guard->commit_graph_->CreateVersionRefAtRoot("Root Version");
  guard->commit_graph_->SetActiveVersionId(new_version);
  guard->serialized_state_needs_writeback_ = true;

  std::string error;
  ASSERT_TRUE(
      pipeline_service.PersistEditorHistoryState(guard, expected_materialized_state, &error))
      << error;
  EXPECT_FALSE(guard->serialized_state_needs_writeback_);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       persisted = graph_service.LoadGraph(715);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->GetActiveVersionId(), new_version);
    EXPECT_EQ(persisted->GetActiveVersionRef().head_commit_hash, std::nullopt);
  }

  pipeline_service.SavePipeline(guard);

  PipelineMgmtService reopened_service(project.GetStorage());
  auto                reopened = reopened_service.LoadEditorPipeline(715);
  ASSERT_NE(reopened, nullptr);
  ASSERT_NE(reopened->commit_graph_, nullptr);
  EXPECT_EQ(reopened->commit_graph_->GetActiveVersionId(), new_version);
  EXPECT_EQ(reopened->working_head_commit_hash(), std::nullopt);
  reopened_service.SavePipeline(reopened);
}

TEST_F(PipelineMapperTests, DeletePipelinesRemovesTheDeletedImagesMiniGitGraphOnly) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  auto                deleted  = pipelines.LoadEditorPipeline(711);
  auto                retained = pipelines.LoadEditorPipeline(712);
  ASSERT_NE(deleted, nullptr);
  ASSERT_NE(retained, nullptr);
  const auto deleted_root  = deleted->root_id_;
  const auto retained_root = retained->root_id_;
  pipelines.SavePipeline(deleted);
  pipelines.SavePipeline(retained);

  const std::vector<sl_element_id_t> deleted_ids = {711};
  pipelines.DeletePipelines(deleted_ids);

  auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  EXPECT_FALSE(graph_service.GetImageEditState(711).has_value());
  EXPECT_FALSE(graph_service.GetRootSerializedPipelineState(711, deleted_root).has_value());
  EXPECT_TRUE(graph_service.LoadGraph(712).has_value());
  EXPECT_TRUE(graph_service.GetRootSerializedPipelineState(712, retained_root).has_value());
}

auto DocumentExposure(const PipelineDocument& document) -> float {
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (exposure == nullptr) {
    throw std::runtime_error("document is missing exposure");
  }
  return exposure->Value();
}

auto MakeExposureBatch(float before, float after) -> PipelineEditBatch {
  nlohmann::json before_json{{"exposure_ev", before}};
  nlohmann::json after_json{{"exposure_ev", after}};
  return MakeSetParameterBatch(test::ColorGradeFieldTarget("exposure"), std::move(before_json),
                               std::move(after_json), true, true, "Default");
}

TEST_F(PipelineMapperTests, ReopenWithStaleCheckpointReplaysFromRoot) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto                initial = first.LoadEditorPipeline(702);
  ASSERT_NE(initial, nullptr);
  const auto root_id = initial->root_id_;
  first.SavePipeline(initial);

  commit_hash_t            expected_head{};
  transaction_chain_hash_t expected_chain{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             graph = graph_service.LoadGraph(702);
    ASSERT_TRUE(graph.has_value());

    auto commit = EditCommit::MakePipelineEdit(graph->GetRootId(), std::nullopt,
                                               MakeExposureBatch(1.5f, 2.0f));
    expected_head = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), expected_head);
    expected_chain = graph->ChainHashForHead(expected_head);

    // Untagged checkpoint JSON. History remains authoritative and must replay.
    graph_service.Materialize(
        graph->CaptureMaterializationWithSerializedPipelineState(nlohmann::json{{"legacy", true}}));
  }

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto                rebuilt = reopened.LoadEditorPipeline(702);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  {
    std::unique_lock<std::mutex> render_lock(rebuilt->pipeline_->GetRenderLock());
    EXPECT_EQ(rebuilt->pipeline_->GpuDagDocument(), rebuilt->document_);
  }
  EXPECT_EQ(rebuilt->root_id_, root_id);
  EXPECT_EQ(rebuilt->working_head_commit_hash(), expected_head);
  EXPECT_EQ(rebuilt->transaction_chain_hash(), expected_chain);
  EXPECT_TRUE(rebuilt->serialized_state_needs_writeback_);
  EXPECT_FLOAT_EQ(DocumentExposure(*rebuilt->document_), 2.0f);
  reopened.SavePipeline(rebuilt);

  PipelineMgmtService after_writeback(project.GetStorage());
  auto                matched = after_writeback.LoadEditorPipeline(702);
  ASSERT_NE(matched, nullptr);
  EXPECT_FALSE(matched->serialized_state_needs_writeback_);
  EXPECT_EQ(matched->working_head_commit_hash(), expected_head);
  EXPECT_EQ(matched->transaction_chain_hash(), expected_chain);
  EXPECT_FLOAT_EQ(DocumentExposure(*matched->document_), 2.0f);
  after_writeback.SavePipeline(matched);
}

/// Insert a commit on the root whose batch cannot apply: its target Color Grade node does not
/// exist, so ReplayPipelineDocumentFromRoot fails at that commit.
auto InsertUnreplayableCommit(CommitGraph& graph) -> commit_hash_t {
  auto missing_target    = test::ColorGradeFieldTarget("exposure");
  missing_target.node_id = NodeId{"grade.does_not_exist"};
  auto commit            = EditCommit::MakePipelineEdit(
      graph.GetRootId(), std::nullopt,
      MakeSetParameterBatch(missing_target, nlohmann::json{{"exposure_ev", 1.5}},
                                       nlohmann::json{{"exposure_ev", 3.0}}, true, true, "missing"));
  const auto hash = commit.GetCommitHash();
  if (!graph.InsertCommit(std::move(commit))) {
    throw std::runtime_error("unreplayable commit was not inserted");
  }
  return hash;
}

auto ExecutorDocument(const PipelineGuard& guard) -> std::shared_ptr<PipelineDocument> {
  std::unique_lock<std::mutex> render_lock(guard.pipeline_->GetRenderLock());
  return guard.pipeline_->GpuDagDocument();
}

TEST_F(PipelineMapperTests, CheckoutReplayFailureKeepsPriorVersionAndDocumentPointer) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto                guard = pipelines.LoadEditorPipeline(741);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  auto&      graph = *guard->commit_graph_;

  const auto bad_version =
      graph.CreateVersionRefAtHead("Unreplayable", InsertUnreplayableCommit(graph));
  const auto prior_version   = graph.GetActiveVersionId();
  const auto prior_document  = guard->document_;
  const auto prior_json      = prior_document->ToJson().dump();
  const bool prior_dirty     = guard->dirty_;
  const bool prior_writeback = guard->serialized_state_needs_writeback_;
  ASSERT_NE(bad_version, prior_version);

  std::string error;
  EXPECT_FALSE(pipelines.CheckoutVersion(guard, bad_version, &error));
  EXPECT_NE(error.find("grade.does_not_exist"), std::string::npos) << error;
  EXPECT_EQ(graph.GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard->document_, prior_document) << "failed replay must not swap the document";
  EXPECT_EQ(ExecutorDocument(*guard), prior_document) << "renderer must keep the prior document";
  EXPECT_EQ(guard->document_->ToJson().dump(), prior_json);
  EXPECT_EQ(guard->dirty_, prior_dirty);
  EXPECT_EQ(guard->serialized_state_needs_writeback_, prior_writeback);
  pipelines.SavePipeline(guard);
}

TEST_F(PipelineMapperTests, CheckoutSuccessBindsReplayedDocumentAndMarksWriteBack) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto                guard = pipelines.LoadEditorPipeline(742);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  auto&      graph       = *guard->commit_graph_;

  auto       commit      = EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt,
                                                        MakeExposureBatch(kDefaultPipelineExposureEv, 2.5f));
  const auto edited_head = commit.GetCommitHash();
  ASSERT_TRUE(graph.InsertCommit(std::move(commit)));
  const auto edited_version                = graph.CreateVersionRefAtHead("Edited", edited_head);
  const auto root_version                  = graph.GetActiveVersionId();
  const auto prior_document                = guard->document_;
  guard->dirty_                            = false;
  guard->serialized_state_needs_writeback_ = false;

  std::string error;
  ASSERT_TRUE(pipelines.CheckoutVersion(guard, edited_version, &error)) << error;
  EXPECT_EQ(graph.GetActiveVersionId(), edited_version);
  EXPECT_EQ(guard->working_head_commit_hash(), edited_head);
  EXPECT_NE(guard->document_, prior_document) << "checkout binds a newly built document";
  EXPECT_EQ(ExecutorDocument(*guard), guard->document_);
  EXPECT_FLOAT_EQ(DocumentExposure(*guard->document_), 2.5f);
  EXPECT_FLOAT_EQ(DocumentExposure(*prior_document), kDefaultPipelineExposureEv)
      << "the swapped-out document is not changed";
  EXPECT_TRUE(guard->serialized_state_needs_writeback_);
  EXPECT_TRUE(guard->dirty_);

  ASSERT_TRUE(pipelines.CheckoutVersion(guard, root_version, &error)) << error;
  EXPECT_EQ(guard->working_head_commit_hash(), std::nullopt);
  EXPECT_EQ(ExecutorDocument(*guard), guard->document_);
  EXPECT_FLOAT_EQ(DocumentExposure(*guard->document_), kDefaultPipelineExposureEv);
  pipelines.SavePipeline(guard);
}

TEST_F(PipelineMapperTests, ActiveVersionRebuildFailureKeepsPriorDocumentPointer) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto                guard = pipelines.LoadEditorPipeline(743);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  auto& graph = *guard->commit_graph_;

  graph.MoveWorkingHead(graph.GetActiveVersionId(), InsertUnreplayableCommit(graph));
  const auto prior_document                = guard->document_;
  const auto prior_json                    = prior_document->ToJson().dump();
  guard->dirty_                            = false;
  guard->serialized_state_needs_writeback_ = false;

  std::string error;
  EXPECT_FALSE(pipelines.RebuildActiveEditorPipeline(guard, &error));
  EXPECT_NE(error.find("active Version rebuild failed"), std::string::npos) << error;
  EXPECT_NE(error.find("grade.does_not_exist"), std::string::npos) << error;
  EXPECT_EQ(guard->document_, prior_document);
  EXPECT_EQ(ExecutorDocument(*guard), prior_document);
  EXPECT_EQ(guard->document_->ToJson().dump(), prior_json);
  EXPECT_FALSE(guard->dirty_);
  EXPECT_FALSE(guard->serialized_state_needs_writeback_);
  pipelines.SavePipeline(guard);
}

TEST_F(PipelineMapperTests, CheckpointForAnotherImageNeverLoads) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto target = first.LoadEditorPipeline(801);
  auto donor  = first.LoadEditorPipeline(802);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(donor, nullptr);
  first.SavePipeline(target);

  {
    std::unique_lock<std::mutex> render_lock(donor->pipeline_->GetRenderLock());
    dynamic_cast<ExposureModel*>(
        donor->document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
        ->SetValue(3.25f);
  }
  donor->serialized_state_needs_writeback_ = true;
  first.SavePipeline(donor);

  nlohmann::json donor_checkpoint;
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             donor_graph = graph_service.LoadGraph(802);
    ASSERT_TRUE(donor_graph.has_value());
    ASSERT_TRUE(donor_graph->GetImageEditState().serialized_pipeline_state.has_value());
    donor_checkpoint = *donor_graph->GetImageEditState().serialized_pipeline_state;
    EXPECT_TRUE(IsPipelineDocumentCheckpointJson(donor_checkpoint));

    auto target_graph = graph_service.LoadGraph(801);
    ASSERT_TRUE(target_graph.has_value());
    graph_service.Materialize(
        target_graph->CaptureMaterializationWithSerializedPipelineState(donor_checkpoint));
  }

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto loaded = reopened.LoadEditorPipeline(801);
  ASSERT_NE(loaded, nullptr);
  EXPECT_GE(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_FLOAT_EQ(DocumentExposure(*loaded->document_), kDefaultPipelineExposureEv);
  EXPECT_NE(loaded->document_->ToJson().dump(), donor_checkpoint.at("pipeline_document").dump());
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests,
       LoadWithMismatchedCheckpointRebuildsFromHistoryAndIgnoresStalePipelineJsonValues) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto                initial = first.LoadEditorPipeline(732);
  ASSERT_NE(initial, nullptr);
  first.SavePipeline(initial);

  commit_hash_t expected_head{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             graph = graph_service.LoadGraph(732);
    ASSERT_TRUE(graph.has_value());

    auto commit = EditCommit::MakePipelineEdit(graph->GetRootId(), std::nullopt,
                                               MakeExposureBatch(1.5f, 3.25f));
    expected_head = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), expected_head);

    auto stale_document = CreateDefaultPipelineDocument();
    dynamic_cast<ExposureModel*>(stale_document.PrimaryGrade()->FindAdjustmentByType(
                                     type_ids::Exposure()))
        ->SetValue(0.0f);
    graph_service.Materialize(graph->CaptureMaterializationWithSerializedPipelineState(
        EncodePipelineDocumentCheckpoint(Hash128{1, 2}, std::nullopt,
                                         ComputeRootChainHash(Hash128{1, 2}), stale_document)));
  }

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto rebuilt = reopened.LoadEditorPipeline(732);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_EQ(rebuilt->working_head_commit_hash(), expected_head);
  EXPECT_FLOAT_EQ(DocumentExposure(*rebuilt->document_), 3.25f)
      << "rebuild must follow history, not a wrong-root checkpoint document";
  reopened.SavePipeline(rebuilt);
}

TEST_F(PipelineMapperTests, SerializedStateWritebackRejectsAConcurrentMaterializedHistoryChange) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  auto                local = pipelines.LoadEditorPipeline(703);
  ASSERT_NE(local, nullptr);
  ASSERT_NE(local->commit_graph_, nullptr);
  const auto          root_id = local->root_id_;

  const auto local_version = local->commit_graph_->CreateVersionRefAtRoot("Local Writeback");
  auto local_commit =
      EditCommit::MakePipelineEdit(root_id, std::nullopt, MakeExposureBatch(0.0f, 1.0f));
  const auto local_head = local_commit.GetCommitHash();
  ASSERT_TRUE(local->commit_graph_->InsertCommit(std::move(local_commit)));
  local->commit_graph_->MoveWorkingHead(local_version, local_head);
  local->commit_graph_->SetActiveVersionId(local_version);
  local->serialized_state_needs_writeback_ = true;

  commit_hash_t remote_head{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             remote_graph = graph_service.LoadGraph(703);
    ASSERT_TRUE(remote_graph.has_value());

    auto remote_commit =
        EditCommit::MakePipelineEdit(root_id, std::nullopt, MakeExposureBatch(0.0f, 2.0f));
    remote_head = remote_commit.GetCommitHash();
    ASSERT_TRUE(remote_graph->InsertCommit(std::move(remote_commit)));
    remote_graph->MoveWorkingHead(remote_graph->GetActiveVersionId(), remote_head);
    graph_service.Materialize(remote_graph->CaptureMaterialization());
  }

  pipelines.SavePipeline(local);
  EXPECT_TRUE(local->serialized_state_needs_writeback_);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       persisted = graph_service.LoadGraph(703);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->GetActiveVersionRef().head_commit_hash, remote_head);
    EXPECT_NE(persisted->GetActiveVersionRef().head_commit_hash, local_head);
  }

  // The test deliberately leaves the local writeback rejected; do not retry it during teardown.
  local->serialized_state_needs_writeback_ = false;
}

TEST_F(PipelineMapperTests,
       CheckpointMaterializedStateSyncLetsVersionPersistenceGuardAcceptDurableTuple) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                guard = pipeline_service.LoadEditorPipeline(720);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  const auto          root_id = guard->root_id_;

  // Commit an adjustment: the working head advances, but ImageEditState.materialized_*
  // stays at root (MoveWorkingHead never advances materialized state by design).
  auto edit =
      EditCommit::MakePipelineEdit(root_id, std::nullopt, MakeExposureBatch(0.0f, 1.0f));
  const auto new_head = edit.GetCommitHash();
  ASSERT_TRUE(guard->commit_graph_->InsertCommit(std::move(edit)));
  guard->commit_graph_->MoveWorkingHead(guard->commit_graph_->GetActiveVersionId(), new_head);

  // Simulate the save checkpoint: it writes the active head to DuckDB but, like the
  // production checkpoint path, does NOT call ApplyMaterializedState, so the in-memory
  // materialized_* stays at root while DuckDB advances to the working head.
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    graph_service.Materialize(
        guard->commit_graph_->CaptureMaterializationWithSerializedPipelineState(
            nlohmann::json{{"exposure", 1.0f}}));
  }

  // DuckDB now holds the working head; the in-memory graph still reports root.
  commit_hash_t durable_head{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             persisted = graph_service.LoadGraph(720);
    ASSERT_TRUE(persisted.has_value());
    durable_head = persisted->GetImageEditState().materialized_head_commit_hash.value();
    ASSERT_EQ(durable_head, new_head);
  }
  EXPECT_EQ(guard->commit_graph_->GetImageEditState().materialized_head_commit_hash, std::nullopt)
      << "in-memory materialized head must stay stale until the post-checkpoint sync";

  // Fix B: mirror the durable materialization into the in-memory state.
  guard->commit_graph_->MaterializeActiveHeadInMemory();
  EXPECT_EQ(guard->commit_graph_->GetImageEditState().materialized_head_commit_hash, new_head);
  EXPECT_EQ(guard->commit_graph_->GetImageEditState().materialized_transaction_chain_hash,
            guard->transaction_chain_hash());

  // The PersistEditorHistoryState guard now sees DuckDB == expected and accepts the
  // durable tuple. Without the sync it throws "persisted history changed before editor
  // history persistence" — the original fork-from-root-after-edits failure.
  std::string error;
  EXPECT_TRUE(pipeline_service.PersistEditorHistoryState(
      guard, guard->commit_graph_->GetImageEditState(), &error))
      << error;

  pipeline_service.SavePipeline(guard);
}

TEST_F(PipelineMapperTests, ImageRootStoresCompleteDefaultDocumentAndDevelopData) {
  ProjectService         project(db_path_, meta_path_);
  PipelineMgmtService    first(project.GetStorage());

  RawRuntimeColorContext raw_context;
  raw_context.valid_                        = true;
  raw_context.output_in_camera_space_       = true;
  raw_context.camera_make_                  = "Alcedo Camera Co";
  raw_context.camera_model_                 = "Root State Test";
  raw_context.lens_metadata_valid_          = true;
  raw_context.lens_make_                    = "Alcedo Optics";
  raw_context.lens_model_                   = "Fixed 35";
  raw_context.focal_length_mm_              = 35.0f;
  raw_context.color_matrices_valid_         = true;
  raw_context.color_matrix_1_[0]            = 0.625;
  raw_context.dng_warp_rectilinear_present_ = true;
  raw_context.dng_warp_rectilinear_applied_ = true;

  auto initial                              = first.LoadPipeline(704);
  ASSERT_NE(initial, nullptr);
  first.InitializeImageRoot(initial, &raw_context);
  const auto root_id         = initial->root_id_;
  const auto persisted_dump  = initial->document_->ToJson().dump();
  first.SavePipeline(initial);

  auto changed_defaults = CreateDefaultPipelineDocument();
  dynamic_cast<ExposureModel*>(
      changed_defaults.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
      ->SetValue(9.0f);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       encoded = graph_service.GetRootSerializedPipelineState(704, root_id);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_FALSE(encoded->contains("pipeline_params"));
    const auto root = DecodePipelineRootState(*encoded);
    EXPECT_EQ(root.document.ToJson().dump(), persisted_dump);
    EXPECT_NE(root.document.ToJson().dump(), changed_defaults.ToJson().dump());
    EXPECT_EQ(root.document.ToJson().at("format_version"), kPipelineDocumentFormatVersion);
    EXPECT_EQ(root.document.ToJson().at("nodes").size(), 3U);
    EXPECT_EQ((*encoded)["raw_color_context"]["CameraModel"], "Root State Test");
    EXPECT_TRUE((*encoded)["raw_color_context"]["DngWarpRectilinearPresent"]);
    EXPECT_TRUE((*encoded)["raw_color_context"]["DngWarpRectilinearApplied"]);
  }

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadEditorPipeline(704);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->document_->ToJson().dump(), persisted_dump);
  const auto& profile = loaded->document_->Develop()->Params().Params().camera_profile;
  EXPECT_TRUE(profile.color_matrices_valid);
  EXPECT_DOUBLE_EQ(profile.color_matrix_1[0], 0.625);
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, NonRawImageRootBindsWorkingSpaceCameraProfile) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto initial = first.LoadPipeline(711);
  ASSERT_NE(initial, nullptr);
  ASSERT_NE(initial->document_->Develop(), nullptr);
  EXPECT_TRUE(initial->document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_NEAR(initial->document_->Develop()->Params().Params().camera_profile.color_matrix_1[0],
              3.2404542, 1e-6);
  ASSERT_TRUE(
      ResolveDevelopColorTransform(initial->document_->Develop()->Params().Params()).ok);

  first.InitializeImageRoot(initial);
  const auto payload = initial->document_->Develop()->Params().Params();
  EXPECT_TRUE(payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  EXPECT_NEAR(payload.camera_profile.color_matrix_2[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(payload).ok);
  EXPECT_NE(payload.camera_profile.color_matrix_1[0], 0.625);
  first.SavePipeline(initial);

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadEditorPipeline(711);
  ASSERT_NE(loaded, nullptr);
  const auto reopened_payload = loaded->document_->Develop()->Params().Params();
  EXPECT_TRUE(reopened_payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(reopened_payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(reopened_payload).ok);
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, PersistedNonRawDocumentWithoutCameraMatricesBecomesRenderableOnReload) {
  ProjectService project(db_path_, meta_path_);
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    graph_service.CreateRootPipelinePersisted(722, CreateDefaultPipelineDocument(), std::nullopt);
  }
  project.GetStorage()->GetElementStore().UpdatePipelineJsonByElementId(
      722, CreateDefaultPipelineDocument().ToJson());

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       state = graph_service.GetImageEditState(722);
    ASSERT_TRUE(state.has_value());
    const auto encoded = graph_service.GetRootSerializedPipelineState(722, state->root_id);
    ASSERT_TRUE(encoded.has_value());
    const auto root = DecodePipelineRootState(*encoded);
    EXPECT_FALSE(root.raw_color_context.has_value());
    ASSERT_NE(root.document.Develop(), nullptr);
    EXPECT_FALSE(root.document.Develop()->Params().Params().camera_profile.color_matrices_valid);
    EXPECT_FALSE(ResolveDevelopColorTransform(root.document.Develop()->Params().Params()).ok);
  }

  PipelineMgmtService pipelines(project.GetStorage());
  auto                loaded = pipelines.LoadPipeline(722);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_->Develop(), nullptr);
  const auto loaded_payload = loaded->document_->Develop()->Params().Params();
  EXPECT_TRUE(loaded_payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(loaded_payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(loaded_payload).ok);

  auto editor = pipelines.LoadEditorPipeline(722);
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(editor->document_->Develop(), nullptr);
  const auto editor_payload = editor->document_->Develop()->Params().Params();
  EXPECT_TRUE(editor_payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(editor_payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(editor_payload).ok);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       encoded =
        graph_service.GetRootSerializedPipelineState(722, editor->root_id_);
    ASSERT_TRUE(encoded.has_value());
    const auto root = DecodePipelineRootState(*encoded);
    EXPECT_FALSE(root.raw_color_context.has_value());
    ASSERT_NE(root.document.Develop(), nullptr);
    EXPECT_FALSE(root.document.Develop()->Params().Params().camera_profile.color_matrices_valid);
  }

  pipelines.SavePipeline(editor);
}

TEST_F(PipelineMapperTests, PersistedRawRootWithoutMatricesDoesNotReceiveWorkingSpaceProfile) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  RawRuntimeColorContext raw_context;
  raw_context.valid_                = true;
  raw_context.color_matrices_valid_ = false;

  auto initial = first.LoadPipeline(723);
  ASSERT_NE(initial, nullptr);
  first.InitializeImageRoot(initial, &raw_context);
  EXPECT_FALSE(initial->document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(initial->document_->Develop()->Params().Params()).ok);
  first.SavePipeline(initial);

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadPipeline(723);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_->Develop(), nullptr);
  EXPECT_FALSE(loaded->document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(loaded->document_->Develop()->Params().Params()).ok);

  auto editor = reopened.LoadEditorPipeline(723);
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(editor->document_->Develop(), nullptr);
  EXPECT_FALSE(editor->document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(editor->document_->Develop()->Params().Params()).ok);
  reopened.SavePipeline(editor);
}

TEST_F(PipelineMapperTests, RootStateRejectsDifferentImageOwner) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  auto                first  = pipelines.LoadEditorPipeline(705);
  auto                second = pipelines.LoadEditorPipeline(706);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  EXPECT_THROW(graph_service.GetRootSerializedPipelineState(706, first->root_id_),
               std::runtime_error);
  db_lock.unlock();
  pipelines.SavePipeline(first);
  pipelines.SavePipeline(second);
}

TEST_F(PipelineMapperTests, SyncPipelineDoesNotPersistUnrelatedDirtyGuards) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  auto                requested = pipelines.LoadPipeline(707);
  auto                unrelated = pipelines.LoadPipeline(708);
  ASSERT_NE(requested, nullptr);
  ASSERT_NE(unrelated, nullptr);
  requested->dirty_ = true;
  unrelated->dirty_ = true;

  pipelines.SyncPipeline(707);
  EXPECT_FALSE(requested->dirty_);
  EXPECT_TRUE(unrelated->dirty_);

  pipelines.SavePipeline(requested);
  unrelated->dirty_ = false;
  pipelines.SavePipeline(unrelated);
}

TEST_F(PipelineMapperTests, EditorLoadReportsMissingReachableCommit) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());
  auto                initial = first.LoadEditorPipeline(703);
  ASSERT_NE(initial, nullptr);
  first.SavePipeline(initial);

  commit_hash_t missing_hash{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             graph = graph_service.LoadGraph(703);
    ASSERT_TRUE(graph.has_value());

    auto commit = EditCommit::MakePipelineEdit(graph->GetRootId(), std::nullopt,
                                               MakeExposureBatch(1.5f, 2.0f));
    missing_hash = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), missing_hash);
    graph_service.Materialize(graph->CaptureMaterialization());

    duckdb_result result;
    ASSERT_EQ(duckdb_query(db_guard.conn_,
                           std::format("DELETE FROM EditCommit WHERE commit_hash='{}';",
                                       missing_hash.ToString())
                               .c_str(),
                           &result),
              DuckDBSuccess);
    duckdb_destroy_result(&result);
  }

  PipelineMgmtService reopened(project.GetStorage());
  try {
    (void)reopened.LoadEditorPipeline(703);
    FAIL() << "expected missing first-parent commit to reject editor open";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("missing"), std::string::npos);
  }
}
}  // namespace alcedo
