//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
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
    RegisterAllOperators();
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
  EXPECT_EQ(guard->document_->ToJson().at("format_version"), 2);

  guard->dirty_ = true;
  pipeline_service.SavePipeline(guard);
  const auto stored = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(9001);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->at("format_version"), 2);
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

    // Modify the pipeline
    auto&          stage = pipeline_guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
    nlohmann::json exp_params;
    exp_params["exposure"] = 1.5f;
    stage.SetOperator(OperatorType::EXPOSURE, exp_params);
    pipeline_guard->dirty_ = true;

    // Save it back
    pipeline_service.SavePipeline(pipeline_guard);

    // Since SavePipeline() will only write back to the cache, we need to call Sync() to write to DB
    pipeline_service.Sync();

    // Load it again and serialize the pipeline to compare
    auto pipeline_guard_2 = pipeline_service.LoadPipeline(1);
    EXPECT_NE(pipeline_guard_2, nullptr);
    EXPECT_EQ(pipeline_guard_2->id_, 1);
    EXPECT_EQ(pipeline_guard_2->pinned_, true);
    EXPECT_EQ(pipeline_guard_2->dirty_,
              false);  // We have sync the cache, so it should not be dirty
    // Serialize it
    pipeline_param = pipeline_guard_2->pipeline_->ExportPipelineParams().dump(2);
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
    // Serialize it
    auto pipeline_param_2 = pipeline_guard->pipeline_->ExportPipelineParams().dump(2);
    EXPECT_EQ(pipeline_param, pipeline_param_2);
  }
}

TEST_F(PipelineMapperTests, DefaultOutputTransformUsesOpenDRT) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                pipeline_guard = pipeline_service.LoadPipeline(42);
  ASSERT_NE(pipeline_guard, nullptr);

  const nlohmann::json exported = pipeline_guard->pipeline_->ExportPipelineParams();
  ASSERT_TRUE(exported.contains("Output Transform"));
  ASSERT_TRUE(exported["Output Transform"].contains("Output Transform"));
  ASSERT_TRUE(exported["Output Transform"]["Output Transform"].contains("odt"));
  ASSERT_TRUE(exported["Output Transform"]["Output Transform"]["odt"].contains("params"));
  ASSERT_TRUE(exported["Output Transform"]["Output Transform"]["odt"]["params"].contains("odt"));
  const auto& odt = exported["Output Transform"]["Output Transform"]["odt"]["params"]["odt"];
  EXPECT_EQ(odt["method"], "open_drt");
  EXPECT_EQ(odt["encoding_eotf"], "gamma_2_2");
  EXPECT_EQ(odt["limiting_space"], "rec709");
  EXPECT_TRUE(odt.contains("open_drt"));

  pipeline_guard->dirty_ = true;
  pipeline_service.SavePipeline(pipeline_guard);
  pipeline_service.Sync();

  auto reloaded = pipeline_service.LoadPipeline(42);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(exported.dump(), reloaded->pipeline_->ExportPipelineParams().dump());
}

TEST_F(PipelineMapperTests, DefaultPipelineAdjustmentsUseCleanBaseline) {
  CPUPipelineExecutor exec;

  const auto          exported = exec.ExportPipelineParams();
  EXPECT_EQ(exported["Basic Adjustment"]["Basic Adjustment"]["exposure"]["params"]["exposure"],
            1.5);
  EXPECT_EQ(exported["Basic Adjustment"]["Basic Adjustment"]["contrast"]["params"]["contrast"],
            0.0);
  EXPECT_EQ(exported["Color Adjustment"]["Color Adjustment"]["saturation"]["params"]["saturation"],
            30.0);
  EXPECT_EQ(exported["Color Adjustment"]["Color Adjustment"]["ocio_lmt"]["params"]["ocio_lmt"], "");
  EXPECT_FALSE(
      exported["Geometry Adjustment"]["Geometry Adjustment"]["crop_rotate"]["enable"].get<bool>());
  EXPECT_EQ(exported["Geometry Adjustment"]["Geometry Adjustment"]["crop_rotate"]["params"]
                    ["crop_rotate"]["enabled"],
            false);
  EXPECT_EQ(exported["Output Transform"]["Output Transform"]["odt"]["params"]["odt"]["method"],
            "open_drt");

  const auto& global = exec.GetGlobalParams();
  EXPECT_FLOAT_EQ(global.exposure_offset_, 1.5f / 17.52f);
  EXPECT_TRUE(global.contrast_enabled_);
  EXPECT_FLOAT_EQ(global.contrast_scale_, 4.0f);
  EXPECT_FLOAT_EQ(global.saturation_offset_, 1.3f);
  EXPECT_FALSE(global.lmt_enabled_);
}

TEST_F(PipelineMapperTests, ResetToCleanBaselineAdjustmentsPreservesLoadingAndColorTemp) {
  CPUPipelineExecutor exec;
  auto&               loading                 = exec.GetStage(PipelineStageName::Image_Loading);
  auto&               to_ws                   = exec.GetStage(PipelineStageName::To_WorkingSpace);

  nlohmann::json      raw_params              = pipeline_defaults::MakeDefaultRawDecodeParams();
  raw_params["raw"]["highlights_reconstruct"] = false;
  loading.SetOperator(OperatorType::RAW_DECODE, raw_params);

  nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "custom"}, {"cct", 7200.0f}, {"tint", 12.0f}}}};
  to_ws.SetOperator(OperatorType::COLOR_TEMP, color_temp_params, exec.GetGlobalParams());

  exec.GetStage(PipelineStageName::Basic_Adjustment)
      .SetOperator(OperatorType::EXPOSURE, {{"exposure", 2.0f}}, exec.GetGlobalParams());
  exec.GetStage(PipelineStageName::Color_Adjustment)
      .SetOperator(OperatorType::SATURATION, {{"saturation", 55.0f}}, exec.GetGlobalParams());

  exec.ResetToCleanBaselineAdjustments();

  const auto exported = exec.ExportPipelineParams();
  EXPECT_EQ(exported["Image Loading"]["Image Loading"]["raw_decode"]["params"]["raw"]
                    ["highlights_reconstruct"],
            false);
  EXPECT_EQ(exported["To Working Space"]["To Working Space"]["color_temp"]["params"]["color_temp"]
                    ["mode"],
            "custom");
  EXPECT_EQ(exported["Basic Adjustment"]["Basic Adjustment"]["exposure"]["params"]["exposure"],
            1.5);
  EXPECT_EQ(exported["Color Adjustment"]["Color Adjustment"]["saturation"]["params"]["saturation"],
            30.0);
}

TEST_F(PipelineMapperTests, LoadPipelineRepairsLensCalibEnableMismatchFromParams) {
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    auto                pipeline_guard = pipeline_service.LoadPipeline(44);
    ASSERT_NE(pipeline_guard, nullptr);

    nlohmann::json serialized = pipeline_guard->pipeline_->ExportPipelineParams();
    auto&          lens_entry = serialized["Image Loading"]["Image Loading"]["lens_calib"];
    ASSERT_TRUE(lens_entry.is_object());
    ASSERT_TRUE(lens_entry.contains("params"));
    ASSERT_TRUE(lens_entry["params"].contains("lens_calib"));

    lens_entry["enable"]                          = true;
    lens_entry["params"]["lens_calib"]["enabled"] = false;
    pipeline_guard->pipeline_->ImportPipelineParams(serialized);

    auto op = pipeline_guard->pipeline_->GetStage(PipelineStageName::Image_Loading)
                  .GetOperator(OperatorType::LENS_CALIBRATION);
    ASSERT_TRUE(op.has_value());
    ASSERT_NE(op.value(), nullptr);
    ASSERT_TRUE(op.value()->enable_);
    ASSERT_FALSE(op.value()->op_->GetParams()["lens_calib"].value("enabled", true));

    pipeline_guard->dirty_ = true;
    pipeline_service.SavePipeline(pipeline_guard);
    pipeline_service.Sync();
  }

  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService pipeline_service(project.GetStorage());

    auto                reloaded = pipeline_service.LoadPipeline(44);
    ASSERT_NE(reloaded, nullptr);
    auto op = reloaded->pipeline_->GetStage(PipelineStageName::Image_Loading)
                  .GetOperator(OperatorType::LENS_CALIBRATION);
    ASSERT_TRUE(op.has_value());
    ASSERT_NE(op.value(), nullptr);
    EXPECT_FALSE(op.value()->enable_);
    EXPECT_FALSE(op.value()->op_->GetParams()["lens_calib"].value("enabled", true));
  }
}

TEST_F(PipelineMapperTests, OutputTransformPersistencePreservesSharedAndMethodSpecificSettings) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipeline_service(project.GetStorage());

  auto                pipeline_guard = pipeline_service.LoadPipeline(43);
  ASSERT_NE(pipeline_guard, nullptr);

  nlohmann::json odt_params                             = pipeline_defaults::MakeDefaultODTParams();
  odt_params["odt"]["method"]                           = "aces_2_0";
  odt_params["odt"]["encoding_space"]                   = "rec2020";
  odt_params["odt"]["encoding_eotf"]                    = "st2084";
  odt_params["odt"]["peak_luminance"]                   = 600.0f;
  odt_params["odt"]["limiting_space"]                   = "p3_d65";
  odt_params["odt"]["open_drt"]["look_preset"]          = "umbra";
  odt_params["odt"]["open_drt"]["tonescale_preset"]     = "aces_2_0";
  odt_params["odt"]["open_drt"]["creative_white"]       = "d60";
  odt_params["odt"]["open_drt"]["creative_white_limit"] = 23.5f;
  odt_params["odt"]["open_drt"]["display_grey_luminance"] = 12.5f;

  auto& output_stage = pipeline_guard->pipeline_->GetStage(PipelineStageName::Output_Transform);
  output_stage.SetOperator(OperatorType::ODT, odt_params);

  pipeline_guard->dirty_ = true;
  pipeline_service.SavePipeline(pipeline_guard);
  pipeline_service.Sync();

  auto reloaded = pipeline_service.LoadPipeline(43);
  ASSERT_NE(reloaded, nullptr);

  const nlohmann::json exported = reloaded->pipeline_->ExportPipelineParams();
  const auto& odt = exported["Output Transform"]["Output Transform"]["odt"]["params"]["odt"];
  EXPECT_EQ(odt["method"], "aces_2_0");
  EXPECT_EQ(odt["encoding_space"], "rec2020");
  EXPECT_EQ(odt["encoding_eotf"], "st2084");
  EXPECT_EQ(odt["peak_luminance"], 600.0);
  EXPECT_EQ(odt["limiting_space"], "p3_d65");
  EXPECT_EQ(odt["open_drt"]["look_preset"], "umbra");
  EXPECT_EQ(odt["open_drt"]["tonescale_preset"], "aces_2_0");
  EXPECT_EQ(odt["open_drt"]["creative_white"], "d60");
  EXPECT_EQ(odt["open_drt"]["creative_white_limit"], 23.5);
  EXPECT_EQ(odt["open_drt"]["display_grey_luminance"], 12.5);
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

      // Modify the pipeline
      auto& stage = pipeline_guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
      nlohmann::json exp_params;
      exp_params["contrast"] = static_cast<float>(i) * 0.5f;
      stage.SetOperator(OperatorType::CONTRAST, exp_params);
      pipeline_guard->dirty_ = true;

      // Save it back
      pipeline_service.SavePipeline(pipeline_guard);
      pipeline_params[i - 1] = pipeline_guard->pipeline_->ExportPipelineParams().dump(2);
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

      // Serialize it
      auto pipeline_param_2 = pipeline_guard->pipeline_->ExportPipelineParams().dump(2);
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

      // Modify the pipeline
      auto& stage         = pipeline_guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
      nlohmann::json exp_params;
      exp_params["exposure"] = static_cast<float>(i) * 0.3f;
      stage.SetOperator(OperatorType::EXPOSURE, exp_params);
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

      // Modify the pipeline
      auto& stage     = pipeline_guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
      nlohmann::json exp_params;
      exp_params["contrast"] = static_cast<float>(i) * 0.4f;
      stage.SetOperator(OperatorType::CONTRAST, exp_params);
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
    const auto empty_dump = CPUPipelineExecutor().ExportPipelineParams().dump();

    for (int i = 0; i < kOpsCount; ++i) {
      const auto id = static_cast<sl_element_id_t>(id_dist(rng));
      const auto op = op_dist(rng);

      if (op == 0) {
        // Load pipeline (cache hit/miss paths)
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        EXPECT_EQ(guard->id_, id);
        auto dump = guard->pipeline_->ExportPipelineParams().dump();
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
        auto&          stage = guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
        nlohmann::json params;
        params["exposure"] = static_cast<float>(id) + value_dist(rng);
        stage.SetOperator(OperatorType::EXPOSURE, params);
        guard->dirty_ = true;
        pipeline_service.SavePipeline(guard);
        expected_dump[id] = guard->pipeline_->ExportPipelineParams().dump();
      } else if (op == 2) {
        // Load + modify without save (pinned & dirty in cache)
        auto guard = pipeline_service.LoadPipeline(id);
        ASSERT_NE(guard, nullptr);
        auto&          stage = guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
        nlohmann::json params;
        params["contrast"] = static_cast<float>(id) + value_dist(rng);
        stage.SetOperator(OperatorType::CONTRAST, params);
        guard->dirty_     = true;
        expected_dump[id] = guard->pipeline_->ExportPipelineParams().dump();
      } else if (op == 3) {
        // Sync all dirty pipelines
        pipeline_service.Sync();
      } else if (op == 4) {
        // Stress eviction by accessing a far ID
        auto guard = pipeline_service.LoadPipeline(static_cast<sl_element_id_t>(kIdRange + id));
        ASSERT_NE(guard, nullptr);
        EXPECT_EQ(guard->id_, static_cast<sl_element_id_t>(kIdRange + id));
        auto       dump   = guard->pipeline_->ExportPipelineParams().dump();
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
        auto serialized = guard->pipeline_->ExportPipelineParams().dump();
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
      auto serialized = guard->pipeline_->ExportPipelineParams().dump();
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
        auto&          stage = guard->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
        nlohmann::json params;
        params["exposure"] = static_cast<float>(id) + static_cast<float>(t) * 0.01f;
        stage.SetOperator(OperatorType::EXPOSURE, params);
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

  const auto empty_dump = CPUPipelineExecutor().ExportPipelineParams().dump();
  for (sl_element_id_t id = 1; id <= 10; ++id) {
    auto guard = pipeline_service.LoadPipeline(id);
    ASSERT_NE(guard, nullptr);
    auto serialized = guard->pipeline_->ExportPipelineParams().dump();
    EXPECT_NE(serialized, empty_dump);
  }
}

// Phase 3: LoadPipelineSnapshot must clone the current pipeline params into an
// independent executor WITHOUT pinning the live guard, clearing dirty state, or
// writing storage. A later user edit must persist through the normal editor path
// and not be overwritten by the snapshot.
TEST_F(PipelineMapperTests, LoadPipelineSnapshotClonesParamsAndDoesNotTouchLiveGuard) {
  nlohmann::json params_v1;
  nlohmann::json params_v2;
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService ps(project.GetStorage());

    auto                g1 = ps.LoadPipeline(1);
    ASSERT_NE(g1, nullptr);
    ASSERT_NE(g1->pipeline_, nullptr);

    // Simulate an unsaved editor edit: exposure = 1.5, guard dirty.
    auto&          stage = g1->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
    nlohmann::json exp1;
    exp1["exposure"] = 1.5f;
    stage.SetOperator(OperatorType::EXPOSURE, exp1);
    g1->dirty_ = true;
    params_v1  = g1->pipeline_->ExportPipelineParams();

    // Capture a snapshot of the current (dirty, pinned) live state.
    std::string err;
    auto        snap = ps.LoadPipelineSnapshot(1, 0, &err);
    ASSERT_NE(snap, nullptr);
    ASSERT_NE(snap->executor_, nullptr);
    EXPECT_NE(snap->executor_, g1->pipeline_);  // independent instance
    EXPECT_NE(&snap->executor_->GetRenderLock(), &g1->pipeline_->GetRenderLock());
    EXPECT_EQ(snap->pipeline_params_, params_v1);                   // captured current params
    EXPECT_EQ(snap->executor_->ExportPipelineParams(), params_v1);  // snapshot holds them

    // Acceptance: the live guard is untouched by the capture.
    EXPECT_EQ(g1->dirty_, true);                                  // dirty NOT cleared
    EXPECT_EQ(g1->pin_count_, size_t{1});                         // pin NOT changed
    EXPECT_EQ(g1->pipeline_->ExportPipelineParams(), params_v1);  // live exec unchanged

    ps.ReleasePipelineSnapshot(snap);
    EXPECT_EQ(g1->dirty_, true);  // release didn't touch live
    EXPECT_EQ(g1->pin_count_, size_t{1});

    // Later user edit after the snapshot. Must persist, not be overwritten.
    nlohmann::json exp2;
    exp2["exposure"] = 2.5f;
    stage.SetOperator(OperatorType::EXPOSURE, exp2);
    g1->dirty_ = true;
    params_v2  = g1->pipeline_->ExportPipelineParams();
    EXPECT_NE(params_v2, params_v1);  // the later edit took effect

    ps.SavePipeline(g1);  // return to cache (last pin)
    ps.Sync();            // write the later edit to storage
  }
  // Re-open from storage and verify the LATER edit (2.5) persisted, not the
  // snapshot's 1.5.
  {
    ProjectService      project(db_path_, meta_path_);
    PipelineMgmtService ps(project.GetStorage());
    auto                g2 = ps.LoadPipeline(1);
    ASSERT_NE(g2, nullptr);
    ASSERT_NE(g2->pipeline_, nullptr);
    EXPECT_EQ(g2->pipeline_->ExportPipelineParams(), params_v2);
  }
}

// Phase 3: cache-miss fallback. When the element is not currently loaded, the
// snapshot path falls back to LoadPipeline/SavePipeline (net-zero pin) to obtain
// a coherent, repaired executor. Must not crash or write storage.
TEST_F(PipelineMapperTests, LoadPipelineSnapshotFallbackRepairsAndReleasesPin) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService ps(project.GetStorage());

  // 999 was never loaded → cache-miss fallback inside LoadPipelineSnapshot.
  std::string         err;
  auto                snap = ps.LoadPipelineSnapshot(999, 0, &err);
  ASSERT_NE(snap, nullptr);
  ASSERT_NE(snap->executor_, nullptr);

  // The fallback guard is in cache unpinned (pin_count_ == 0) — re-loading must
  // re-pin it cleanly without throwing.
  auto g = ps.LoadPipeline(999);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->pinned_, true);
  EXPECT_EQ(g->pin_count_, size_t{1});
  ps.SavePipeline(g);

  EXPECT_NO_THROW(ps.ReleasePipelineSnapshot(snap));
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
  const auto expected_params = initial->pipeline_->ExportPipelineParams();
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
  EXPECT_EQ(loaded->pipeline_->ExportPipelineParams(), expected_params);
  reopened.SavePipeline(loaded);
}

TEST_F(PipelineMapperTests, LoadWithMatchingCheckpointSkipsFullReplay) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService first(project.GetStorage());

  auto                initial = first.LoadEditorPipeline(731);
  ASSERT_NE(initial, nullptr);
  first.SavePipeline(initial);

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto loaded = reopened.LoadEditorPipeline(731);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u)
      << "matching checkpoint identity must import serialized state without history rebuild";
  EXPECT_FALSE(loaded->serialized_state_needs_writeback_);
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

TEST_F(PipelineMapperTests, StaleSerializedStateRebuildsAndIsWrittenBack) {
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

    OrdinaryEditPayload payload;
    payload.operator_type  = OperatorType::EXPOSURE;
    payload.stage_name     = PipelineStageName::Basic_Adjustment;
    payload.field_name     = "exposure";
    payload.before_value   = 1.5f;
    payload.after_value    = 2.0f;
    payload.before_enabled = true;
    payload.after_enabled  = true;
    auto commit   = EditCommit::MakeEdit(graph->GetRootId(), std::nullopt, std::move(payload));
    expected_head = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), expected_head);
    expected_chain = graph->ChainHashForHead(expected_head);

    // This is an untagged serialized state. Its graph state remains valid, but the editor must
    // reject it and replay the new first-parent commit from the immutable root.
    graph_service.Materialize(
        graph->CaptureMaterializationWithSerializedPipelineState(nlohmann::json{{"legacy", true}}));
  }

  PipelineMgmtService reopened(project.GetStorage());
  auto                rebuilt = reopened.LoadEditorPipeline(702);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_EQ(rebuilt->root_id_, root_id);
  EXPECT_EQ(rebuilt->working_head_commit_hash(), expected_head);
  EXPECT_EQ(rebuilt->transaction_chain_hash(), expected_chain);
  EXPECT_TRUE(rebuilt->serialized_state_needs_writeback_);
  EXPECT_EQ(rebuilt->pipeline_->ExportPipelineParams()["Basic Adjustment"]["Basic Adjustment"]
                                                      ["exposure"]["params"]["exposure"],
            2.0f);
  reopened.SavePipeline(rebuilt);

  PipelineMgmtService after_writeback(project.GetStorage());
  auto                matched = after_writeback.LoadEditorPipeline(702);
  ASSERT_NE(matched, nullptr);
  EXPECT_FALSE(matched->serialized_state_needs_writeback_);
  EXPECT_EQ(matched->working_head_commit_hash(), expected_head);
  EXPECT_EQ(matched->transaction_chain_hash(), expected_chain);
  EXPECT_EQ(matched->pipeline_->ExportPipelineParams()["Basic Adjustment"]["Basic Adjustment"]
                                                      ["exposure"]["params"]["exposure"],
            2.0f);
  after_writeback.SavePipeline(matched);
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

    OrdinaryEditPayload payload;
    payload.operator_type  = OperatorType::EXPOSURE;
    payload.stage_name     = PipelineStageName::Basic_Adjustment;
    payload.field_name     = "exposure";
    payload.before_value   = 0.0f;
    payload.after_value    = 3.25f;
    payload.before_enabled = true;
    payload.after_enabled  = true;
    auto commit   = EditCommit::MakeEdit(graph->GetRootId(), std::nullopt, std::move(payload));
    expected_head = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), expected_head);

    // Deliberately wrong params under a non-matching checkpoint identity.
    graph_service.Materialize(graph->CaptureMaterializationWithSerializedPipelineState(
        nlohmann::json{{"legacy", true}, {"stale_exposure", 0.0f}}));
  }

  PipelineMgmtService reopened(project.GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto rebuilt = reopened.LoadEditorPipeline(732);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 1u);
  EXPECT_EQ(rebuilt->working_head_commit_hash(), expected_head);
  EXPECT_EQ(rebuilt->pipeline_->ExportPipelineParams()["Basic Adjustment"]["Basic Adjustment"]
                                                      ["exposure"]["params"]["exposure"],
            3.25f)
      << "rebuild must follow history, not stale checkpoint JSON values";
  reopened.SavePipeline(rebuilt);
}

TEST_F(PipelineMapperTests, SerializedStateWritebackRejectsAConcurrentMaterializedHistoryChange) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());

  auto                local = pipelines.LoadEditorPipeline(703);
  ASSERT_NE(local, nullptr);
  ASSERT_NE(local->commit_graph_, nullptr);
  const auto          root_id = local->root_id_;

  OrdinaryEditPayload local_payload;
  local_payload.operator_type  = OperatorType::EXPOSURE;
  local_payload.stage_name     = PipelineStageName::Basic_Adjustment;
  local_payload.field_name     = "$operator_params";
  local_payload.before_value   = nlohmann::json{{"exposure", 0.0f}};
  local_payload.after_value    = nlohmann::json{{"exposure", 1.0f}};
  local_payload.before_enabled = true;
  local_payload.after_enabled  = true;
  const auto local_version     = local->commit_graph_->CreateVersionRefAtRoot("Local Writeback");
  auto       local_commit = EditCommit::MakeEdit(root_id, std::nullopt, std::move(local_payload));
  const auto local_head   = local_commit.GetCommitHash();
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

    OrdinaryEditPayload remote_payload;
    remote_payload.operator_type  = OperatorType::EXPOSURE;
    remote_payload.stage_name     = PipelineStageName::Basic_Adjustment;
    remote_payload.field_name     = "$operator_params";
    remote_payload.before_value   = nlohmann::json{{"exposure", 0.0f}};
    remote_payload.after_value    = nlohmann::json{{"exposure", 2.0f}};
    remote_payload.before_enabled = true;
    remote_payload.after_enabled  = true;
    auto remote_commit = EditCommit::MakeEdit(root_id, std::nullopt, std::move(remote_payload));
    remote_head        = remote_commit.GetCommitHash();
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
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "exposure";
  payload.before_value   = 0.0f;
  payload.after_value    = 1.0f;
  payload.before_enabled = true;
  payload.after_enabled  = true;
  auto       edit        = EditCommit::MakeEdit(root_id, std::nullopt, std::move(payload));
  const auto new_head    = edit.GetCommitHash();
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

TEST_F(PipelineMapperTests, ImmutableRootRestoresImportedRawColorAndLensState) {
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
  initial->pipeline_->InjectRawMetadata(raw_context);
  first.InitializeImageRoot(initial, &raw_context);
  const auto root_id = initial->root_id_;
  first.SavePipeline(initial);

  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       encoded = graph_service.GetRootSerializedPipelineState(704, root_id);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_TRUE(encoded->contains("raw_color_context"));
    EXPECT_EQ((*encoded)["raw_color_context"]["CameraModel"], "Root State Test");
    EXPECT_TRUE((*encoded)["raw_color_context"]["DngWarpRectilinearPresent"]);
    EXPECT_TRUE((*encoded)["raw_color_context"]["DngWarpRectilinearApplied"]);
  }

  PipelineMgmtService reopened(project.GetStorage());
  auto                loaded = reopened.LoadEditorPipeline(704);
  ASSERT_NE(loaded, nullptr);
  const auto& global = loaded->pipeline_->GetGlobalParams();
  EXPECT_TRUE(global.raw_runtime_valid_);
  EXPECT_EQ(global.raw_camera_make_, "Alcedo Camera Co");
  EXPECT_EQ(global.raw_camera_model_, "Root State Test");
  EXPECT_TRUE(global.raw_color_matrices_valid_);
  EXPECT_DOUBLE_EQ(global.raw_color_matrix_1_[0], 0.625);
  reopened.SavePipeline(loaded);
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

    OrdinaryEditPayload payload;
    payload.operator_type  = OperatorType::EXPOSURE;
    payload.stage_name     = PipelineStageName::Basic_Adjustment;
    payload.field_name     = "exposure";
    payload.before_value   = 1.5f;
    payload.after_value    = 2.0f;
    payload.before_enabled = true;
    payload.after_enabled  = true;
    auto commit  = EditCommit::MakeEdit(graph->GetRootId(), std::nullopt, std::move(payload));
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
