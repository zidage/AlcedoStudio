//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/editor_working_document.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/editor_parameter_target_test.hpp"
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

/// Create the history root of @p element_id from the default document with the working-space
/// camera profile, as import does for an RGB file. The editor opens only images with a root.
void InitializeDefaultRoot(PipelineMgmtService& pipelines, sl_element_id_t element_id) {
  pipelines.InitializeImageRoot(element_id, CreateDefaultPipelineDocument(), nullptr);
}

/// Apply @p batch to the editor's working document and record it as the next commit on the active
/// Version of the lease's graph, as the editor history does for a settled edit.
auto CommitOnLease(EditorHistoryLease& lease, const PipelineEditBatch& batch) -> commit_hash_t {
  std::string error;
  if (!ApplyPipelineEditBatch(*lease.document_, batch, PipelineEditApplyDirection::Forward,
                              &error)) {
    throw std::runtime_error("batch did not apply: " + error);
  }
  auto&      graph  = lease.graph_;
  auto       commit = EditCommit::MakePipelineEdit(graph.GetRootId(),
                                                   graph.GetActiveVersionRef().head_commit_hash, batch);
  const auto head   = commit.GetCommitHash();
  if (!graph.InsertCommit(std::move(commit))) {
    throw std::runtime_error("commit was not inserted");
  }
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  return head;
}

/// Committed snapshot of the lease's working document at the active head, as the editor publishes
/// it after a committed change.
auto CommittedSnapshotOfLease(const EditorHistoryLease& lease, sl_element_id_t element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  const auto head = lease.graph_.GetActiveVersionRef().head_commit_hash;
  return PipelineGraphSnapshot::Committed(std::as_const(*lease.document_).Freeze(), element_id,
                                          PipelineLineageId::Next(), head,
                                          lease.graph_.ChainHashForHead(head));
}

/// Stored image edit state of @p element_id; fails the test when there is none.
auto StoredEditState(ProjectService& project, sl_element_id_t element_id)
    -> std::optional<ImageEditState> {
  auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  return graph_service.GetImageEditState(element_id);
}

TEST_F(PipelineMapperTests, FailedDocumentSaveKeepsDirtyStateAndJournal) {
  constexpr sl_element_id_t element_id = 8506;
  ProjectService           project(db_path_, meta_path_);
  PipelineMgmtService      pipeline_service(project.GetStorage());

  auto                      guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);
  const auto stored_before =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored_before.has_value());

  guard = pipeline_service.LoadPipeline(element_id);
  ASSERT_NE(guard, nullptr);
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
  InitializeDefaultRoot(first, 701);

  auto initial = first.AcquireEditorLease(701);
  ASSERT_NE(initial.document_, nullptr);
  const auto root_id = initial.graph_.GetRootId();
  EXPECT_NE(root_id, Hash128{});
  EXPECT_FALSE(initial.graph_.GetActiveVersionRef().head_commit_hash.has_value());
  EXPECT_EQ(initial.graph_.ChainHashForHead(std::nullopt), ComputeRootChainHash(root_id));
  const auto  expected_document = initial.document_->ToJson();
  std::string error;
  ASSERT_TRUE(first.PersistEditorHistory(initial.graph_, initial.graph_.GetImageEditState(),
                                         *initial.document_, &error))
      << error;
  first.ReleaseEditorLease(701);

  // A new service instance forces the editor path to read the serialized state rather than
  // reusing anything the first service holds.
  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  const auto loaded = reopened.AcquireEditorLease(701);
  ASSERT_NE(loaded.document_, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u);
  EXPECT_EQ(loaded.graph_.GetRootId(), root_id);
  EXPECT_EQ(loaded.graph_.GetActiveVersionRef().head_commit_hash, std::nullopt);
  EXPECT_EQ(loaded.graph_.ChainHashForHead(std::nullopt), ComputeRootChainHash(root_id));
  EXPECT_EQ(loaded.document_->ToJson(), expected_document);
  reopened.ReleaseEditorLease(701);
}

// Regression test of commit 671802168: while the editor holds an image, no other history user may
// replace its history from storage or read storage as its state (that silently dropped the
// editor's unsaved history). Thumbnails and export read what the editor published.
TEST_F(PipelineMapperTests,
       HeldEditorLeaseRefusesStorageHistoryUsersAndReleaseKeepsThePublishedState) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 751);
  // A library writer read the stored history before the editor opened the image.
  const auto  library_base = pipelines.LoadHistorySnapshot(751);
  CommitGraph library_edit = *library_base.graph_;
  {
    auto       commit       = EditCommit::MakePipelineEdit(library_edit.GetRootId(), std::nullopt,
                                                           MakeExposureBatch(kDefaultPipelineExposureEv, 3.0f));
    const auto library_head = commit.GetCommitHash();
    ASSERT_TRUE(library_edit.InsertCommit(std::move(commit)));
    library_edit.MoveWorkingHead(library_edit.GetActiveVersionId(), library_head);
  }

  auto lease = pipelines.AcquireEditorLease(751);
  ASSERT_NE(lease.document_, nullptr);
  EXPECT_THROW((void)pipelines.AcquireEditorLease(751), std::runtime_error);
  EXPECT_THROW((void)pipelines.LoadHistorySnapshot(751), std::runtime_error);
  EXPECT_THROW((void)pipelines.PersistHistory(library_base, library_edit), std::runtime_error);

  // The editor commits a change and publishes it before the history reaches storage.
  const auto head      = CommitOnLease(lease, MakeExposureBatch(kDefaultPipelineExposureEv, 2.5f));
  const auto published = CommittedSnapshotOfLease(lease, 751);
  pipelines.PublishCommitted(published);
  ASSERT_EQ(StoredEditState(project, 751)->materialized_head_commit_hash, std::nullopt);
  const auto committed = pipelines.AcquireCommittedSnapshot(751);
  EXPECT_EQ(committed, published) << "the held image renders what the editor published";
  EXPECT_EQ(committed->Head(), head);
  EXPECT_FLOAT_EQ(DocumentExposure(committed->Document()), 2.5f);

  std::string error;
  ASSERT_TRUE(pipelines.PersistEditorHistory(lease.graph_, lease.graph_.GetImageEditState(),
                                             *lease.document_, &error))
      << error;
  pipelines.ReleaseEditorLease(751);

  const auto element_json = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(751);
  ASSERT_TRUE(element_json.has_value());
  EXPECT_EQ(*element_json, published->Document().ToJson())
      << "release writes the last published committed document as the element pipeline JSON";

  pipelines.ResetEditorPipelineHistoryRebuildCountForTesting();
  const auto reopened = pipelines.AcquireEditorLease(751);
  ASSERT_NE(reopened.document_, nullptr);
  EXPECT_EQ(pipelines.EditorPipelineHistoryRebuildCount(), 0u);
  EXPECT_EQ(reopened.graph_.GetActiveVersionRef().head_commit_hash, head);
  EXPECT_EQ(reopened.document_->ToJson(), published->Document().ToJson());
  pipelines.ReleaseEditorLease(751);
}

TEST_F(PipelineMapperTests, ReopenWithMatchingCheckpointSkipsReplay) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());
  InitializeDefaultRoot(first, 731);

  auto initial = first.AcquireEditorLease(731);
  ASSERT_NE(initial.document_, nullptr);
  const auto  expected_document = initial.document_->ToJson();
  std::string error;
  ASSERT_TRUE(first.PersistEditorHistory(initial.graph_, initial.graph_.GetImageEditState(),
                                         *initial.document_, &error))
      << error;
  first.ReleaseEditorLease(731);

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
  const auto loaded = reopened.AcquireEditorLease(731);
  ASSERT_NE(loaded.document_, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u)
      << "matching checkpoint identity must import serialized state without history rebuild";
  EXPECT_EQ(loaded.document_->ToJson(), expected_document);
  reopened.ReleaseEditorLease(731);
}

TEST_F(PipelineMapperTests, PersistEditorHistoryWritesNewActiveVersionBeforeEditorReopen) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());
  InitializeDefaultRoot(pipeline_service, 715);

  auto lease = pipeline_service.AcquireEditorLease(715);
  ASSERT_NE(lease.document_, nullptr);
  const auto expected_materialized_state = lease.graph_.GetImageEditState();

  const auto new_version                 = lease.graph_.CreateVersionRefAtRoot("Root Version");
  lease.graph_.SetActiveVersionId(new_version);

  std::string error;
  ASSERT_TRUE(pipeline_service.PersistEditorHistory(lease.graph_, expected_materialized_state,
                                                    lease.root_->document, &error))
      << error;
  EXPECT_EQ(lease.graph_.GetImageEditState().active_version_id, new_version);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       persisted = graph_service.LoadGraph(715);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->GetActiveVersionId(), new_version);
    EXPECT_EQ(persisted->GetActiveVersionRef().head_commit_hash, std::nullopt);
  }

  pipeline_service.ReleaseEditorLease(715);

  PipelineMgmtService reopened_service(project.GetStorage());
  const auto          reopened = reopened_service.AcquireEditorLease(715);
  EXPECT_EQ(reopened.graph_.GetActiveVersionId(), new_version);
  EXPECT_EQ(reopened.graph_.GetActiveVersionRef().head_commit_hash, std::nullopt);
  reopened_service.ReleaseEditorLease(715);
}

TEST_F(PipelineMapperTests, DeletePipelinesRemovesTheDeletedImagesMiniGitGraphOnly) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  InitializeDefaultRoot(pipelines, 711);
  InitializeDefaultRoot(pipelines, 712);
  const auto deleted_root  = pipelines.LoadHistorySnapshot(711).graph_->GetRootId();
  const auto retained_root = pipelines.LoadHistorySnapshot(712).graph_->GetRootId();

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

TEST_F(PipelineMapperTests, ReopenWithStaleCheckpointReplaysFromRoot) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());
  InitializeDefaultRoot(first, 702);
  const auto               root_id = first.LoadHistorySnapshot(702).graph_->GetRootId();

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
  auto rebuilt = reopened.AcquireEditorLease(702);
  ASSERT_NE(rebuilt.document_, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_EQ(rebuilt.graph_.GetRootId(), root_id);
  EXPECT_EQ(rebuilt.graph_.GetActiveVersionRef().head_commit_hash, expected_head);
  EXPECT_EQ(rebuilt.graph_.ChainHashForHead(expected_head), expected_chain);
  EXPECT_FLOAT_EQ(DocumentExposure(*rebuilt.document_), 2.0f);
  // The editor writes the checkpoint of the replayed document with its next history write.
  std::string error;
  ASSERT_TRUE(reopened.PersistEditorHistory(rebuilt.graph_, rebuilt.graph_.GetImageEditState(),
                                            *rebuilt.document_, &error))
      << error;
  reopened.ReleaseEditorLease(702);

  PipelineMgmtService after_writeback(project.GetStorage());
  after_writeback.ResetEditorPipelineHistoryRebuildCountForTesting();
  const auto matched = after_writeback.AcquireEditorLease(702);
  ASSERT_NE(matched.document_, nullptr);
  EXPECT_EQ(after_writeback.EditorPipelineHistoryRebuildCount(), 0u);
  EXPECT_EQ(matched.graph_.GetActiveVersionRef().head_commit_hash, expected_head);
  EXPECT_EQ(matched.graph_.ChainHashForHead(expected_head), expected_chain);
  EXPECT_FLOAT_EQ(DocumentExposure(*matched.document_), 2.0f);
  after_writeback.ReleaseEditorLease(702);
}

// Version checkout builds the target document privately; a replay failure reports the failing
// commit and changes neither the graph nor the working document.
TEST_F(PipelineMapperTests, VersionReplayFailureReportsTheFailingCommitAndChangesNoEditorState) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 741);
  auto       lease          = pipelines.AcquireEditorLease(741);
  auto&      graph          = lease.graph_;

  const auto bad_head       = InsertUnreplayableCommit(graph);
  const auto bad_version    = graph.CreateVersionRefAtHead("Unreplayable", bad_head);
  const auto prior_version  = graph.GetActiveVersionId();
  const auto prior_document = lease.document_;
  const auto prior_json     = prior_document->ToJson().dump();
  const auto prior_state    = StoredEditState(project, 741);
  ASSERT_NE(bad_version, prior_version);

  std::string error;
  EXPECT_EQ(BuildDocumentFromRoot(graph, *lease.root_, bad_head, &error), nullptr);
  EXPECT_NE(error.find("grade.does_not_exist"), std::string::npos) << error;
  EXPECT_EQ(graph.GetActiveVersionId(), prior_version);
  EXPECT_EQ(lease.document_, prior_document) << "failed replay must not swap the document";
  EXPECT_EQ(lease.document_->ToJson().dump(), prior_json);
  const auto state_after = StoredEditState(project, 741);
  ASSERT_TRUE(prior_state.has_value() && state_after.has_value());
  EXPECT_EQ(state_after->ToJSON(), prior_state->ToJSON());
  pipelines.ReleaseEditorLease(741);
}

// Version checkout: the replayed document of the target head becomes the working document in a
// new lineage, and the history write stores it as the checkpoint of the new active head.
TEST_F(PipelineMapperTests, VersionCheckoutPersistsTheReplayedDocumentAsTheCheckpointOfTheNewHead) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 742);
  auto                  lease = pipelines.AcquireEditorLease(742);
  auto&                 graph = lease.graph_;
  EditorWorkingDocument working(742, lease.document_);

  auto       commit      = EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt,
                                                        MakeExposureBatch(kDefaultPipelineExposureEv, 2.5f));
  const auto edited_head = commit.GetCommitHash();
  ASSERT_TRUE(graph.InsertCommit(std::move(commit)));
  const auto edited_version = graph.CreateVersionRefAtHead("Edited", edited_head);
  const auto root_version   = graph.GetActiveVersionId();
  const auto prior_lineage  = working.Lineage();

  const auto checkout       = [&](version_ref_id_t version, head_commit_hash_t head) {
    std::string error;
    auto        built = BuildDocumentFromRoot(graph, *lease.root_, head, &error);
    ASSERT_NE(built, nullptr) << error;
    const auto expected = graph.GetImageEditState();
    graph.SetActiveVersionId(version);
    ASSERT_TRUE(pipelines.PersistEditorHistory(graph, expected, *built, &error)) << error;
    working.Replace(std::move(built));
    (void)working.PublishPreview();
  };

  checkout(edited_version, edited_head);
  EXPECT_EQ(graph.GetActiveVersionId(), edited_version);
  EXPECT_NE(working.Lineage(), prior_lineage) << "checkout releases the prior document's binding";
  EXPECT_FLOAT_EQ(DocumentExposure(working.Document()), 2.5f);
  EXPECT_FLOAT_EQ(DocumentExposure(*lease.document_), kDefaultPipelineExposureEv)
      << "the swapped-out document is not changed";
  EXPECT_EQ(working.CurrentPreview()->Document().ToJson(), working.Document().ToJson());
  {
    const auto state = StoredEditState(project, 742);
    ASSERT_TRUE(state.has_value() && state->serialized_pipeline_state.has_value());
    EXPECT_EQ(state->active_version_id, edited_version);
    EXPECT_EQ(state->materialized_head_commit_hash, edited_head);
    const auto checkpoint = DecodePipelineDocumentCheckpoint(*state->serialized_pipeline_state);
    EXPECT_EQ(checkpoint.head_commit_hash, edited_head);
    EXPECT_EQ(checkpoint.transaction_chain_hash, graph.ChainHashForHead(edited_head));
    EXPECT_EQ(checkpoint.document.ToJson(), working.Document().ToJson())
        << "the checkpoint equals the document of the new active head";
  }

  const auto edited_lineage = working.Lineage();
  checkout(root_version, std::nullopt);
  EXPECT_EQ(graph.GetActiveVersionRef().head_commit_hash, std::nullopt);
  EXPECT_NE(working.Lineage(), edited_lineage);
  EXPECT_FLOAT_EQ(DocumentExposure(working.Document()), kDefaultPipelineExposureEv);
  {
    const auto state = StoredEditState(project, 742);
    ASSERT_TRUE(state.has_value() && state->serialized_pipeline_state.has_value());
    EXPECT_EQ(state->active_version_id, root_version);
    const auto checkpoint = DecodePipelineDocumentCheckpoint(*state->serialized_pipeline_state);
    EXPECT_EQ(checkpoint.head_commit_hash, std::nullopt);
    EXPECT_EQ(checkpoint.document.ToJson(), working.Document().ToJson());
  }
  pipelines.ReleaseEditorLease(742);
}

// The editor opens an image whose stored active head cannot be replayed: the open fails with the
// replay error, and the lease is not left held.
TEST_F(PipelineMapperTests, EditorLeaseOfAnUnreplayableActiveHeadFailsWithTheReplayError) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 743);
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             graph = graph_service.LoadGraph(743);
    ASSERT_TRUE(graph.has_value());
    graph->MoveWorkingHead(graph->GetActiveVersionId(), InsertUnreplayableCommit(*graph));
    graph_service.Materialize(graph->CaptureMaterialization());
  }

  try {
    (void)pipelines.AcquireEditorLease(743);
    FAIL() << "an unreplayable active head must fail the editor open";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("grade.does_not_exist"), std::string::npos)
        << error.what();
  }
  EXPECT_NO_THROW((void)pipelines.LoadHistorySnapshot(743)) << "the failed open holds no lease";
}

TEST_F(PipelineMapperTests, CheckpointForAnotherImageNeverLoads) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());
  InitializeDefaultRoot(first, 801);
  InitializeDefaultRoot(first, 802);

  {
    auto donor = first.AcquireEditorLease(802);
    dynamic_cast<ExposureModel*>(
        donor.document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
        ->SetValue(3.25f);
    std::string error;
    ASSERT_TRUE(first.PersistEditorHistory(donor.graph_, donor.graph_.GetImageEditState(),
                                           *donor.document_, &error))
        << error;
    first.ReleaseEditorLease(802);
  }

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
  const auto loaded = reopened.AcquireEditorLease(801);
  ASSERT_NE(loaded.document_, nullptr);
  EXPECT_GE(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_FLOAT_EQ(DocumentExposure(*loaded.document_), kDefaultPipelineExposureEv);
  EXPECT_NE(loaded.document_->ToJson().dump(), donor_checkpoint.at("pipeline_document").dump());
  reopened.ReleaseEditorLease(801);
}

TEST_F(PipelineMapperTests,
       LoadWithMismatchedCheckpointRebuildsFromHistoryAndIgnoresStalePipelineJsonValues) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());
  InitializeDefaultRoot(first, 732);

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
  const auto rebuilt = reopened.AcquireEditorLease(732);
  ASSERT_NE(rebuilt.document_, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_EQ(rebuilt.graph_.GetActiveVersionRef().head_commit_hash, expected_head);
  EXPECT_FLOAT_EQ(DocumentExposure(*rebuilt.document_), 3.25f)
      << "rebuild must follow history, not a wrong-root checkpoint document";
  reopened.ReleaseEditorLease(732);
}

TEST_F(PipelineMapperTests, EditorHistoryPersistenceRejectsAConcurrentMaterializedHistoryChange) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 703);

  auto       local          = pipelines.AcquireEditorLease(703);
  const auto root_id        = local.graph_.GetRootId();
  const auto expected_state = local.graph_.GetImageEditState();

  const auto local_version  = local.graph_.CreateVersionRefAtRoot("Local Writeback");
  auto       local_commit =
      EditCommit::MakePipelineEdit(root_id, std::nullopt, MakeExposureBatch(0.0f, 1.0f));
  const auto local_head = local_commit.GetCommitHash();
  ASSERT_TRUE(local.graph_.InsertCommit(std::move(local_commit)));
  local.graph_.MoveWorkingHead(local_version, local_head);
  local.graph_.SetActiveVersionId(local_version);

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

  const auto  local_state = local.graph_.GetImageEditState();
  std::string error;
  EXPECT_FALSE(
      pipelines.PersistEditorHistory(local.graph_, expected_state, *local.document_, &error));
  EXPECT_NE(error.find("persisted history changed"), std::string::npos) << error;
  EXPECT_EQ(local.graph_.GetImageEditState().ToJSON(), local_state.ToJSON())
      << "a rejected write leaves the graph's materialized state unchanged";

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       persisted = graph_service.LoadGraph(703);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->GetActiveVersionRef().head_commit_hash, remote_head);
    EXPECT_NE(persisted->GetActiveVersionRef().head_commit_hash, local_head);
  }
  pipelines.ReleaseEditorLease(703);
}

TEST_F(PipelineMapperTests,
       CheckpointMaterializedStateSyncLetsVersionPersistenceAcceptDurableTuple) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());
  InitializeDefaultRoot(pipeline_service, 720);

  auto       lease    = pipeline_service.AcquireEditorLease(720);
  auto&      graph    = lease.graph_;

  // Commit an adjustment: the working head advances, but ImageEditState.materialized_*
  // stays at root (MoveWorkingHead never advances materialized state by design).
  const auto new_head = CommitOnLease(lease, MakeExposureBatch(kDefaultPipelineExposureEv, 1.0f));

  // Simulate a checkpoint write that stores the active head in DuckDB but does NOT call
  // ApplyMaterializedState, so the in-memory materialized_* stays at root while DuckDB advances
  // to the working head.
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    graph_service.Materialize(graph.CaptureMaterializationWithSerializedPipelineState(
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
  EXPECT_EQ(graph.GetImageEditState().materialized_head_commit_hash, std::nullopt)
      << "in-memory materialized head must stay stale until the post-checkpoint sync";

  // Fix B: mirror the durable materialization into the in-memory state.
  graph.MaterializeActiveHeadInMemory();
  EXPECT_EQ(graph.GetImageEditState().materialized_head_commit_hash, new_head);
  EXPECT_EQ(graph.GetImageEditState().materialized_transaction_chain_hash,
            graph.ChainHashForHead(new_head));

  // The PersistEditorHistory check now sees DuckDB == expected and accepts the durable tuple.
  // Without the sync it fails with "persisted history changed before editor history
  // persistence", the original fork-from-root-after-edits failure.
  std::string error;
  EXPECT_TRUE(pipeline_service.PersistEditorHistory(graph, graph.GetImageEditState(),
                                                    *lease.document_, &error))
      << error;
  pipeline_service.ReleaseEditorLease(720);
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

  first.InitializeImageRoot(704, CreateDefaultPipelineDocument(), &raw_context);
  // The element pipeline JSON is the root document, written for older application versions.
  const auto element_json = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(704);
  ASSERT_TRUE(element_json.has_value());
  const auto persisted_dump = PipelineDocument::FromJson(*element_json).ToJson().dump();
  root_id_t  root_id{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       state = graph_service.GetImageEditState(704);
    ASSERT_TRUE(state.has_value());
    root_id = state->root_id;
  }

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
  const auto          loaded = reopened.AcquireEditorLease(704);
  ASSERT_NE(loaded.document_, nullptr);
  EXPECT_EQ(loaded.document_->ToJson().dump(), persisted_dump);
  const auto& profile = loaded.document_->Develop()->Params().Params().camera_profile;
  EXPECT_TRUE(profile.color_matrices_valid);
  EXPECT_DOUBLE_EQ(profile.color_matrix_1[0], 0.625);
  reopened.ReleaseEditorLease(704);
}

TEST_F(PipelineMapperTests, NonRawImageRootBindsWorkingSpaceCameraProfile) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  first.InitializeImageRoot(711, CreateDefaultPipelineDocument(), nullptr);
  const auto root = first.LoadHistorySnapshot(711).root_;
  ASSERT_NE(root, nullptr);
  EXPECT_FALSE(root->raw_color_context.has_value());
  ASSERT_NE(root->document.Develop(), nullptr);
  const auto payload = root->document.Develop()->Params().Params();
  EXPECT_TRUE(payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  EXPECT_NEAR(payload.camera_profile.color_matrix_2[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(payload).ok);
  EXPECT_NE(payload.camera_profile.color_matrix_1[0], 0.625);

  PipelineMgmtService reopened(project.GetStorage());
  const auto          loaded = reopened.AcquireEditorLease(711);
  ASSERT_NE(loaded.document_, nullptr);
  const auto reopened_payload = loaded.document_->Develop()->Params().Params();
  EXPECT_TRUE(reopened_payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(reopened_payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(reopened_payload).ok);
  reopened.ReleaseEditorLease(711);
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

  const auto editor = pipelines.AcquireEditorLease(722);
  ASSERT_NE(editor.document_, nullptr);
  ASSERT_NE(editor.document_->Develop(), nullptr);
  const auto editor_payload = editor.document_->Develop()->Params().Params();
  EXPECT_TRUE(editor_payload.camera_profile.color_matrices_valid);
  EXPECT_NEAR(editor_payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  ASSERT_TRUE(ResolveDevelopColorTransform(editor_payload).ok);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       encoded =
        graph_service.GetRootSerializedPipelineState(722, editor.graph_.GetRootId());
    ASSERT_TRUE(encoded.has_value());
    const auto root = DecodePipelineRootState(*encoded);
    EXPECT_FALSE(root.raw_color_context.has_value());
    ASSERT_NE(root.document.Develop(), nullptr);
    EXPECT_FALSE(root.document.Develop()->Params().Params().camera_profile.color_matrices_valid);
  }

  pipelines.ReleaseEditorLease(722);
  pipelines.ReleasePipelineUse(loaded);
}

// Editor open of an image whose RAW root exists must not bind the working-space Rec.709 profile
// onto the editor's document: its renders would use the wrong colors. The document carries the
// RAW camera profile of the root, and the root stays as stored.
TEST_F(PipelineMapperTests, EditorOpenOfExistingRawRootKeepsTheRawCameraProfile) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  RawRuntimeColorContext raw_context;
  raw_context.valid_                  = true;
  raw_context.output_in_camera_space_ = true;
  raw_context.color_matrices_valid_   = true;
  raw_context.color_matrix_1_[0]      = 0.625;
  raw_context.color_matrix_2_[0]      = 0.5;

  pipelines.InitializeImageRoot(724, CreateDefaultPipelineDocument(), &raw_context);
  const auto root_id = pipelines.LoadHistorySnapshot(724).graph_->GetRootId();

  const auto lease   = pipelines.AcquireEditorLease(724);
  ASSERT_NE(lease.document_, nullptr);
  const auto profile = lease.document_->Develop()->Params().Params().camera_profile;
  EXPECT_TRUE(profile.color_matrices_valid);
  EXPECT_DOUBLE_EQ(profile.color_matrix_1[0], 0.625);
  EXPECT_DOUBLE_EQ(profile.color_matrix_2[0], 0.5);
  EXPECT_EQ(lease.graph_.GetRootId(), root_id);
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       encoded = graph_service.GetRootSerializedPipelineState(724, root_id);
    ASSERT_TRUE(encoded.has_value());
    const auto root = DecodePipelineRootState(*encoded);
    ASSERT_TRUE(root.raw_color_context.has_value());
    EXPECT_DOUBLE_EQ(root.document.Develop()->Params().Params().camera_profile.color_matrix_1[0],
                     0.625);
  }
  pipelines.ReleaseEditorLease(724);
}

TEST_F(PipelineMapperTests, PersistedRawRootWithoutMatricesDoesNotReceiveWorkingSpaceProfile) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  RawRuntimeColorContext raw_context;
  raw_context.valid_                = true;
  raw_context.color_matrices_valid_ = false;

  first.InitializeImageRoot(723, CreateDefaultPipelineDocument(), &raw_context);
  const auto root = first.LoadHistorySnapshot(723).root_;
  ASSERT_NE(root, nullptr);
  EXPECT_FALSE(root->document.Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(root->document.Develop()->Params().Params()).ok);

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadPipeline(723);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_->Develop(), nullptr);
  EXPECT_FALSE(loaded->document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(loaded->document_->Develop()->Params().Params()).ok);

  const auto editor = reopened.AcquireEditorLease(723);
  ASSERT_NE(editor.document_, nullptr);
  ASSERT_NE(editor.document_->Develop(), nullptr);
  EXPECT_FALSE(editor.document_->Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(ResolveDevelopColorTransform(editor.document_->Develop()->Params().Params()).ok);
  reopened.ReleaseEditorLease(723);
  reopened.ReleasePipelineUse(loaded);
}

TEST_F(PipelineMapperTests, RootStateRejectsDifferentImageOwner) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  InitializeDefaultRoot(pipelines, 705);
  InitializeDefaultRoot(pipelines, 706);
  const auto       first_root = pipelines.LoadHistorySnapshot(705).graph_->GetRootId();

  auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  EXPECT_THROW(graph_service.GetRootSerializedPipelineState(706, first_root), std::runtime_error);
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
  InitializeDefaultRoot(first, 703);

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
    (void)reopened.AcquireEditorLease(703);
    FAIL() << "expected missing first-parent commit to reject editor open";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("missing"), std::string::npos);
  }
}

TEST_F(PipelineMapperTests, EditorLeaseOfAnImageWithoutHistoryRootFailsWithTheRealError) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  try {
    (void)pipelines.AcquireEditorLease(761);
    FAIL() << "an image without a history root must not open in the editor";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("has no edit history root"), std::string::npos)
        << error.what();
  }
  EXPECT_FALSE(StoredEditState(project, 761).has_value())
      << "the failed open must not create a root";

  // The failed open does not hold the lease: once the image has a root, it opens.
  InitializeDefaultRoot(pipelines, 761);
  const auto lease = pipelines.AcquireEditorLease(761);
  ASSERT_NE(lease.document_, nullptr);
  EXPECT_EQ(lease.graph_.GetElementId(), 761u);
  pipelines.ReleaseEditorLease(761);
}

TEST_F(PipelineMapperTests, PersistEditorHistoryWritesHistoryAndCheckpointInOneTransaction) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 762);

  auto        lease          = pipelines.AcquireEditorLease(762);
  const auto  expected_state = lease.graph_.GetImageEditState();
  const auto  head  = CommitOnLease(lease, MakeExposureBatch(kDefaultPipelineExposureEv, 2.25f));
  const auto  chain = lease.graph_.ChainHashForHead(head);

  std::string error;
  ASSERT_TRUE(
      pipelines.PersistEditorHistory(lease.graph_, expected_state, *lease.document_, &error))
      << error;
  EXPECT_EQ(lease.graph_.GetImageEditState().materialized_head_commit_hash, head)
      << "a successful write advances the graph's materialized state";

  const auto stored = StoredEditState(project, 762);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->materialized_head_commit_hash, head);
  EXPECT_EQ(stored->materialized_transaction_chain_hash, chain);
  ASSERT_TRUE(stored->serialized_pipeline_state.has_value());
  const auto checkpoint = DecodePipelineDocumentCheckpoint(*stored->serialized_pipeline_state);
  EXPECT_EQ(checkpoint.root_id, lease.graph_.GetRootId());
  EXPECT_EQ(checkpoint.head_commit_hash, head);
  EXPECT_EQ(checkpoint.transaction_chain_hash, chain);
  EXPECT_EQ(checkpoint.document.ToJson(), lease.document_->ToJson());
  EXPECT_FLOAT_EQ(DocumentExposure(checkpoint.document), 2.25f);
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       persisted = graph_service.LoadGraph(762);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->GetActiveVersionRef().head_commit_hash, head);
  }

  // A second write that still expects the state before the first one is stale: it fails and
  // leaves storage as the first write left it.
  auto stale_document = ClonePipelineDocument(*lease.document_);
  dynamic_cast<ExposureModel*>(
      stale_document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
      ->SetValue(4.0f);
  CommitGraph stale_graph = lease.graph_;
  const auto  stale_head  = [&] {
    auto commit =
        EditCommit::MakePipelineEdit(stale_graph.GetRootId(), head, MakeExposureBatch(2.25f, 4.0f));
    const auto hash = commit.GetCommitHash();
    EXPECT_TRUE(stale_graph.InsertCommit(std::move(commit)));
    stale_graph.MoveWorkingHead(stale_graph.GetActiveVersionId(), hash);
    return hash;
  }();
  const auto stale_graph_state = stale_graph.GetImageEditState();
  error.clear();
  EXPECT_FALSE(pipelines.PersistEditorHistory(stale_graph, expected_state, stale_document, &error));
  EXPECT_NE(error.find("persisted history changed"), std::string::npos) << error;
  EXPECT_EQ(stale_graph.GetImageEditState().ToJSON(), stale_graph_state.ToJSON());
  const auto after_stale = StoredEditState(project, 762);
  ASSERT_TRUE(after_stale.has_value());
  EXPECT_EQ(after_stale->ToJSON(), stored->ToJSON()) << "a rejected write changes no storage";
  EXPECT_NE(after_stale->materialized_head_commit_hash, stale_head);
  pipelines.ReleaseEditorLease(762);
}

TEST_F(PipelineMapperTests, PersistEditorHistoryRefusesAnImageTheEditorDoesNotHold) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  InitializeDefaultRoot(pipelines, 763);

  auto lease = pipelines.AcquireEditorLease(763);
  pipelines.ReleaseEditorLease(763);
  const auto stored_before  = StoredEditState(project, 763);
  const auto expected_state = lease.graph_.GetImageEditState();
  (void)CommitOnLease(lease, MakeExposureBatch(kDefaultPipelineExposureEv, 2.0f));
  const auto  graph_state = lease.graph_.GetImageEditState();

  std::string error;
  EXPECT_FALSE(
      pipelines.PersistEditorHistory(lease.graph_, expected_state, *lease.document_, &error));
  EXPECT_NE(error.find("is not held by an editor session"), std::string::npos) << error;
  EXPECT_EQ(lease.graph_.GetImageEditState().ToJSON(), graph_state.ToJSON());
  const auto stored_after = StoredEditState(project, 763);
  ASSERT_TRUE(stored_before.has_value() && stored_after.has_value());
  EXPECT_EQ(stored_after->ToJSON(), stored_before->ToJSON());
}
}  // namespace alcedo
