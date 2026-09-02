//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <span>
#include <string>

#include "app/history_mgmt_service.hpp"
#include "app/project_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto OperatorParamsFor(CPUPipelineExecutor& pipeline, PipelineStageName stage_name,
                       OperatorType op_type) -> nlohmann::json {
  auto entry = pipeline.GetStage(stage_name).GetOperator(op_type);
  if (!entry.has_value() || entry.value() == nullptr || !entry.value()->op_) {
    return nlohmann::json::object();
  }
  return entry.value()->op_->GetParams();
}

auto PackageContainsOperator(const AdjustmentTransferPackage& package, OperatorType op_type)
    -> bool {
  for (const auto& entry : package.operators_) {
    if (entry.operator_type_ == op_type) {
      return true;
    }
  }
  return false;
}

auto MakeCustomOdtParams() -> nlohmann::json {
  auto params = pipeline_defaults::MakeDefaultODTParams();
  auto& odt   = params["odt"];
  odt["method"]                                        = "open_drt";
  odt["encoding_space"]                                = "rec2020";
  odt["encoding_eotf"]                                 = "st2084";
  odt["peak_luminance"]                                = 400.0f;
  odt["open_drt"]["look_preset"]                       = "umbra";
  odt["open_drt"]["tonescale_preset"]                  = "aces_2_0";
  odt["open_drt"]["creative_white"]                    = "d60";
  odt["open_drt"]["creative_white_limit"]              = 0.42f;
  odt["open_drt"]["display_grey_luminance"]            = 14.0f;
  odt["open_drt"]["hdr_grey_boost"]                    = 0.25f;
  odt["open_drt"]["hdr_purity"]                        = 0.7f;
  odt["open_drt"]["parameters"]["tn_con"]              = 1.85f;
  odt["open_drt"]["parameters"]["ptm_high"]            = -0.65f;
  return params;
}

class AdjustmentTransferServiceTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    const auto suffix = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    db_path_ = std::filesystem::temp_directory_path() /
               ("adjustment_transfer_service_test_" + suffix + ".db");
    meta_path_ = std::filesystem::temp_directory_path() /
                 ("adjustment_transfer_service_test_" + suffix + ".json");
    std::filesystem::remove(db_path_);
    std::filesystem::remove(meta_path_);
  }

  void TearDown() override {
    std::filesystem::remove(db_path_);
    std::filesystem::remove(meta_path_);
  }
};

TEST_F(AdjustmentTransferServiceTest, DefaultCaptureTransfersUserAdjustmentsOnly) {
  CPUPipelineExecutor source;
  CPUPipelineExecutor target;

  auto&               source_global = source.GetGlobalParams();
  source.GetStage(PipelineStageName::Basic_Adjustment)
      .SetOperator(OperatorType::EXPOSURE, {{"exposure", 2.25f}}, source_global);

  source.GetStage(PipelineStageName::To_WorkingSpace)
      .SetOperator(OperatorType::COLOR_TEMP,
                   {{"color_temp",
                     {{"mode", "as_shot"},
                      {"custom_cct", 4300.0f},
                      {"custom_tint", 15.0f},
                      {"as_shot_cct", 4300.0f},
                      {"as_shot_tint", 15.0f}}}},
                   source_global);

  auto& loading                               = source.GetStage(PipelineStageName::Image_Loading);
  auto  raw_params                            = pipeline_defaults::MakeDefaultRawDecodeParams();
  raw_params["raw"]["highlights_reconstruct"] = false;
  loading.SetOperator(OperatorType::RAW_DECODE, raw_params);

  auto lens_params                             = pipeline_defaults::MakeDefaultLensCalibParams();
  lens_params["lens_calib"]["enabled"]         = true;
  lens_params["lens_calib"]["lens_model"]      = "Source Lens";
  lens_params["lens_calib"]["focal_length_mm"] = 85.0f;
  loading.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, source_global);
  loading.EnableOperator(OperatorType::LENS_CALIBRATION, true, source_global);

  const auto package = AdjustmentTransferService::Capture(source);
  EXPECT_TRUE(PackageContainsOperator(package, OperatorType::EXPOSURE));
  EXPECT_TRUE(PackageContainsOperator(package, OperatorType::COLOR_TEMP));
  EXPECT_FALSE(PackageContainsOperator(package, OperatorType::RAW_DECODE));
  EXPECT_FALSE(PackageContainsOperator(package, OperatorType::LENS_CALIBRATION));

  const auto exported = AdjustmentTransferService::ExportPackage(package);
  for (const auto& entry : exported["operators"]) {
    if (entry["operator"] == "color_temp") {
      const auto& color_temp = entry["params"]["color_temp"];
      EXPECT_FALSE(color_temp.contains("resolved_cct"));
      EXPECT_FALSE(color_temp.contains("resolved_tint"));
      EXPECT_FALSE(color_temp.contains("as_shot_cct"));
      EXPECT_FALSE(color_temp.contains("as_shot_tint"));
      EXPECT_FALSE(color_temp.contains("cct"));
      EXPECT_FALSE(color_temp.contains("tint"));
      EXPECT_FALSE(color_temp.contains("custom_cct"));
      EXPECT_FALSE(color_temp.contains("custom_tint"));
    }
  }

  EXPECT_TRUE(AdjustmentTransferService::Apply(target, package));

  const auto exposure =
      OperatorParamsFor(target, PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE);
  EXPECT_DOUBLE_EQ(exposure["exposure"].get<double>(), 2.25);

  const auto target_raw =
      OperatorParamsFor(target, PipelineStageName::Image_Loading, OperatorType::RAW_DECODE);
  EXPECT_TRUE(target_raw["raw"]["highlights_reconstruct"].get<bool>());

  const auto target_lens =
      OperatorParamsFor(target, PipelineStageName::Image_Loading, OperatorType::LENS_CALIBRATION);
  EXPECT_NE(target_lens["lens_calib"].value("lens_model", std::string{}), "Source Lens");
  EXPECT_FALSE(target_lens["lens_calib"].value("enabled", true));

  const auto target_color_temp =
      OperatorParamsFor(target, PipelineStageName::To_WorkingSpace, OperatorType::COLOR_TEMP);
  EXPECT_EQ(target_color_temp["color_temp"]["mode"], "as_shot");
  EXPECT_NE(target_color_temp["color_temp"].value("as_shot_cct", 0.0), 4300.0);
}

TEST_F(AdjustmentTransferServiceTest, ImportsExternalJsonWithStableOperatorNames) {
  CPUPipelineExecutor  target;

  const nlohmann::json external_package = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "exposure"}, {"enabled", true}, {"params", {{"exposure", -0.75f}}}},
        {{"operator", "saturation"}, {"params", {{"saturation", 44.0f}}}}}}};

  const auto package = AdjustmentTransferService::ImportPackage(external_package);
  ASSERT_EQ(package.operators_.size(), 2u);
  EXPECT_EQ(package.operators_[0].stage_, PipelineStageName::Basic_Adjustment);
  EXPECT_EQ(package.operators_[1].stage_, PipelineStageName::Color_Adjustment);

  EXPECT_TRUE(AdjustmentTransferService::Apply(target, package));

  const auto exposure =
      OperatorParamsFor(target, PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE);
  const auto saturation =
      OperatorParamsFor(target, PipelineStageName::Color_Adjustment, OperatorType::SATURATION);
  EXPECT_DOUBLE_EQ(exposure["exposure"].get<double>(), -0.75);
  EXPECT_DOUBLE_EQ(saturation["saturation"].get<double>(), 44.0);

  const auto round_trip =
      AdjustmentTransferService::ImportPackage(AdjustmentTransferService::ExportPackage(package));
  ASSERT_EQ(round_trip.operators_.size(), package.operators_.size());
  EXPECT_EQ(round_trip.operators_[0].operator_type_, OperatorType::EXPOSURE);
  EXPECT_EQ(round_trip.operators_[1].operator_type_, OperatorType::SATURATION);
}

TEST_F(AdjustmentTransferServiceTest, MergeParamsUpdatesOnlyNestedRawSetting) {
  CPUPipelineExecutor target;

  auto&               loading                 = target.GetStage(PipelineStageName::Image_Loading);
  auto                raw_params              = pipeline_defaults::MakeDefaultRawDecodeParams();
  raw_params["raw"]["highlights_reconstruct"] = false;
  raw_params["raw"]["use_camera_wb"]          = false;
  raw_params["raw"]["user_wb"]                = 7100;
  loading.SetOperator(OperatorType::RAW_DECODE, raw_params, target.GetGlobalParams());

  const nlohmann::json external_package = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "raw_decode"},
         {"enabled", true},
         {"mergeParams", true},
         {"params", {{"raw", {{"highlights_reconstruct", true}}}}}}}}};

  const auto package = AdjustmentTransferService::ImportPackage(external_package);
  ASSERT_EQ(package.operators_.size(), 1u);
  EXPECT_TRUE(package.operators_[0].merge_params_);

  EXPECT_TRUE(AdjustmentTransferService::Apply(target, package));

  const auto raw =
      OperatorParamsFor(target, PipelineStageName::Image_Loading, OperatorType::RAW_DECODE);
  EXPECT_TRUE(raw["raw"]["highlights_reconstruct"].get<bool>());
  EXPECT_FALSE(raw["raw"]["use_camera_wb"].get<bool>());
  EXPECT_DOUBLE_EQ(raw["raw"]["user_wb"].get<double>(), 7100.0);
}

TEST_F(AdjustmentTransferServiceTest, LensRuntimeMetadataRequiresExplicitSelection) {
  CPUPipelineExecutor source;
  CPUPipelineExecutor target_without_metadata;
  CPUPipelineExecutor target_with_metadata;

  auto&               source_global            = source.GetGlobalParams();
  auto                lens_params              = pipeline_defaults::MakeDefaultLensCalibParams();
  lens_params["lens_calib"]["enabled"]         = true;
  lens_params["lens_calib"]["lens_model"]      = "Source Lens";
  lens_params["lens_calib"]["focal_length_mm"] = 85.0f;

  auto& loading                                = source.GetStage(PipelineStageName::Image_Loading);
  loading.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, source_global);
  loading.EnableOperator(OperatorType::LENS_CALIBRATION, true, source_global);

  AdjustmentTransferSelection lens_settings_only;
  lens_settings_only.include_lens_calibration_ = true;
  const auto settings_package = AdjustmentTransferService::Capture(source, lens_settings_only);
  ASSERT_TRUE(PackageContainsOperator(settings_package, OperatorType::LENS_CALIBRATION));
  EXPECT_TRUE(AdjustmentTransferService::Apply(target_without_metadata, settings_package));
  auto target_lens = OperatorParamsFor(target_without_metadata, PipelineStageName::Image_Loading,
                                       OperatorType::LENS_CALIBRATION);
  EXPECT_NE(target_lens["lens_calib"].value("lens_model", std::string{}), "Source Lens");
  EXPECT_DOUBLE_EQ(target_lens["lens_calib"].value("focal_length_mm", 0.0), 0.0);

  AdjustmentTransferSelection lens_with_metadata;
  lens_with_metadata.include_lens_calibration_                  = true;
  lens_with_metadata.include_lens_calibration_runtime_metadata_ = true;
  const auto metadata_package = AdjustmentTransferService::Capture(source, lens_with_metadata);
  EXPECT_TRUE(AdjustmentTransferService::Apply(target_with_metadata, metadata_package));
  target_lens = OperatorParamsFor(target_with_metadata, PipelineStageName::Image_Loading,
                                  OperatorType::LENS_CALIBRATION);
  EXPECT_EQ(target_lens["lens_calib"].value("lens_model", std::string{}), "Source Lens");
  EXPECT_DOUBLE_EQ(target_lens["lens_calib"].value("focal_length_mm", 0.0), 85.0);
}

TEST_F(AdjustmentTransferServiceTest, VersionedApplyCreatesActiveCheckoutVersion) {
  constexpr sl_element_id_t target_id = 42;

  ProjectService         project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  PipelineMgmtService    pipeline_service(project.GetStorage());
  EditHistoryMgmtService history_service(project.GetStorage());

  {
    auto pipeline = pipeline_service.LoadPipeline(target_id);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->pipeline_, nullptr);
    auto& global = pipeline->pipeline_->GetGlobalParams();
    pipeline->pipeline_->GetStage(PipelineStageName::Basic_Adjustment)
        .SetOperator(OperatorType::EXPOSURE, {{"exposure", 0.0f}}, global);
    pipeline->pipeline_->GetStage(PipelineStageName::Basic_Adjustment)
        .SetOperator(OperatorType::CONTRAST, {{"contrast", 0.0f}}, global);
    pipeline->dirty_ = true;
    pipeline_service.SavePipeline(pipeline);
    pipeline_service.Sync();
  }

  const nlohmann::json external_package = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "exposure"}, {"enabled", true}, {"params", {{"exposure", 1.5f}}}},
        {{"operator", "contrast"}, {"enabled", true}, {"params", {{"contrast", 2.25f}}}}}}};
  const auto package = AdjustmentTransferService::ImportPackage(external_package);

  const auto before_history = history_service.LoadHistory(target_id);
  ASSERT_NE(before_history, nullptr);
  const auto default_version_id = before_history->history_->GetDefaultVersionID();
  history_service.SaveHistory(before_history);

  const sl_element_id_t ids[] = {target_id};
  const auto result = AdjustmentTransferService::Apply(
      pipeline_service, history_service, std::span<const sl_element_id_t>(ids), package,
      "Pasted Adjustments");

  ASSERT_EQ(result.failures_.size(), 0u);
  ASSERT_EQ(result.applied_ids_.size(), 1u);
  EXPECT_EQ(result.applied_ids_[0], target_id);

  auto history = history_service.LoadHistory(target_id);
  ASSERT_NE(history, nullptr);
  ASSERT_NE(history->history_, nullptr);
  EXPECT_EQ(history->history_->GetVersions().size(), 2u);
  EXPECT_NE(history->history_->GetActiveVersionID(), default_version_id);

  auto& active = history->history_->GetActiveVersion();
  EXPECT_EQ(active.GetDisplayName(), "Pasted Adjustments");
  EXPECT_EQ(active.GetAllEditTransactions().size(), 2u);
  EXPECT_EQ(active.GetCursor(), 2u);

  const auto reconstructed =
      history->history_->ReconstructPipelineParamsForVersion(history->history_->GetActiveVersionID());
  ASSERT_TRUE(reconstructed.has_value());

  CPUPipelineExecutor reconstructed_pipeline;
  reconstructed_pipeline.ImportPipelineParams(*reconstructed);
  const auto exposure = OperatorParamsFor(reconstructed_pipeline, PipelineStageName::Basic_Adjustment,
                                          OperatorType::EXPOSURE);
  const auto contrast = OperatorParamsFor(reconstructed_pipeline, PipelineStageName::Basic_Adjustment,
                                          OperatorType::CONTRAST);
  EXPECT_DOUBLE_EQ(exposure["exposure"].get<double>(), 1.5);
  EXPECT_DOUBLE_EQ(contrast["contrast"].get<double>(), 2.25);

  auto pipeline = pipeline_service.LoadPipeline(target_id);
  ASSERT_NE(pipeline, nullptr);
  ASSERT_NE(pipeline->pipeline_, nullptr);
  const auto live_exposure = OperatorParamsFor(*pipeline->pipeline_,
                                               PipelineStageName::Basic_Adjustment,
                                               OperatorType::EXPOSURE);
  EXPECT_DOUBLE_EQ(live_exposure["exposure"].get<double>(), 1.5);
  pipeline_service.SavePipeline(pipeline);
  history_service.SaveHistory(history);

  const nlohmann::json second_external_package = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "exposure"}, {"enabled", true}, {"params", {{"exposure", 2.0f}}}},
        {{"operator", "contrast"}, {"enabled", true}, {"params", {{"contrast", 3.0f}}}}}}};
  const auto second_package = AdjustmentTransferService::ImportPackage(second_external_package);
  const auto second_result = AdjustmentTransferService::Apply(
      pipeline_service, history_service, std::span<const sl_element_id_t>(ids), second_package,
      "Pasted Adjustments");
  ASSERT_EQ(second_result.failures_.size(), 0u);
  ASSERT_EQ(second_result.applied_ids_.size(), 1u);

  auto second_history = history_service.LoadHistory(target_id);
  ASSERT_NE(second_history, nullptr);
  ASSERT_NE(second_history->history_, nullptr);
  EXPECT_EQ(second_history->history_->GetVersions().size(), 3u);
  auto& second_active = second_history->history_->GetActiveVersion();
  EXPECT_EQ(second_active.GetDisplayName(), "Pasted Adjustments (2)");
  EXPECT_EQ(second_active.GetAllEditTransactions().size(), 2u);
  history_service.SaveHistory(second_history);
}

TEST_F(AdjustmentTransferServiceTest, VersionedMergeMaterializesCombinedParamsWithoutEdits) {
  constexpr sl_element_id_t target_id = 42;

  ProjectService         project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  PipelineMgmtService    pipeline_service(project.GetStorage());
  EditHistoryMgmtService history_service(project.GetStorage());

  const sl_element_id_t ids[] = {target_id};
  const nlohmann::json  initial_package_json = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "exposure"}, {"enabled", true}, {"params", {{"exposure", 0.5f}}}},
        {{"operator", "contrast"}, {"enabled", true}, {"params", {{"contrast", 2.0f}}}},
        {{"operator", "saturation"}, {"enabled", true}, {"params", {{"saturation", 12.0f}}}}}}};
  const auto initial_package = AdjustmentTransferService::ImportPackage(initial_package_json);
  const auto initial_result  = AdjustmentTransferService::Apply(
      pipeline_service, history_service, std::span<const sl_element_id_t>(ids), initial_package,
      "Pasted Adjustments");
  ASSERT_EQ(initial_result.failures_.size(), 0u);
  ASSERT_EQ(initial_result.applied_ids_.size(), 1u);

  auto pasted_history = history_service.LoadHistory(target_id);
  ASSERT_NE(pasted_history, nullptr);
  ASSERT_NE(pasted_history->history_, nullptr);
  const auto pasted_version_id = pasted_history->history_->GetActiveVersionID();
  EXPECT_EQ(pasted_history->history_->GetActiveVersion().GetDisplayName(), "Pasted Adjustments");
  EXPECT_EQ(pasted_history->history_->GetActiveVersion().GetAllEditTransactions().size(), 3u);
  history_service.SaveHistory(pasted_history);

  const nlohmann::json merge_package_json = {
      {"schema", "alcedo.adjustment_transfer.v2"},
      {"operators",
       {{{"operator", "exposure"}, {"enabled", true}, {"params", {{"exposure", 1.5f}}}},
        {{"operator", "contrast"}, {"enabled", true}, {"params", {{"contrast", -0.25f}}}}}}};
  const auto merge_package = AdjustmentTransferService::ImportPackage(merge_package_json);
  const auto merge_result  = AdjustmentTransferService::Apply(
      pipeline_service, history_service, std::span<const sl_element_id_t>(ids), merge_package,
      "Merged Adjustments", AdjustmentVersionApplyMode::kMerge);
  ASSERT_EQ(merge_result.failures_.size(), 0u);
  ASSERT_EQ(merge_result.applied_ids_.size(), 1u);

  auto history = history_service.LoadHistory(target_id);
  ASSERT_NE(history, nullptr);
  ASSERT_NE(history->history_, nullptr);
  EXPECT_EQ(history->history_->GetVersions().size(), 3u);

  auto& merged = history->history_->GetActiveVersion();
  EXPECT_EQ(merged.GetDisplayName(), "Merged Adjustments");
  EXPECT_EQ(merged.GetAllEditTransactions().size(), 0u);
  EXPECT_EQ(merged.GetCursor(), 0u);
  EXPECT_EQ(merged.GetTransactionCount(), 0u);
  EXPECT_TRUE(merged.GetFinalPipelineParams().has_value());

  const auto reconstructed = history->history_->ReconstructPipelineParamsForVersion(
      history->history_->GetActiveVersionID());
  ASSERT_TRUE(reconstructed.has_value());
  CPUPipelineExecutor reconstructed_pipeline;
  reconstructed_pipeline.ImportPipelineParams(*reconstructed);

  const auto exposure = OperatorParamsFor(
      reconstructed_pipeline, PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE);
  const auto contrast = OperatorParamsFor(
      reconstructed_pipeline, PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST);
  const auto saturation = OperatorParamsFor(
      reconstructed_pipeline, PipelineStageName::Color_Adjustment, OperatorType::SATURATION);
  EXPECT_DOUBLE_EQ(exposure["exposure"].get<double>(), 1.5);
  EXPECT_DOUBLE_EQ(contrast["contrast"].get<double>(), -0.25);
  EXPECT_DOUBLE_EQ(saturation["saturation"].get<double>(), 12.0);

  auto& pasted = history->history_->GetVersion(pasted_version_id);
  EXPECT_EQ(pasted.GetDisplayName(), "Pasted Adjustments");
  EXPECT_EQ(pasted.GetAllEditTransactions().size(), 3u);
  history_service.SaveHistory(history);
}

TEST_F(AdjustmentTransferServiceTest, VersionedApplyPersistsOutputTransformForEditorReopen) {
  constexpr sl_element_id_t source_id = 41;
  constexpr sl_element_id_t target_id = 42;

  ProjectService         project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  PipelineMgmtService    pipeline_service(project.GetStorage());
  EditHistoryMgmtService history_service(project.GetStorage());

  {
    auto source_pipeline = pipeline_service.LoadPipeline(source_id);
    ASSERT_NE(source_pipeline, nullptr);
    ASSERT_NE(source_pipeline->pipeline_, nullptr);
    auto& global = source_pipeline->pipeline_->GetGlobalParams();
    source_pipeline->pipeline_->GetStage(PipelineStageName::Output_Transform)
        .SetOperator(OperatorType::ODT, MakeCustomOdtParams(), global);
    source_pipeline->pipeline_->GetStage(PipelineStageName::Output_Transform)
        .EnableOperator(OperatorType::ODT, true, global);
    source_pipeline->dirty_ = true;
    pipeline_service.SavePipeline(source_pipeline);
  }

  {
    auto target_pipeline = pipeline_service.LoadPipeline(target_id);
    ASSERT_NE(target_pipeline, nullptr);
    ASSERT_NE(target_pipeline->pipeline_, nullptr);
    target_pipeline->dirty_ = true;
    pipeline_service.SavePipeline(target_pipeline);
  }
  pipeline_service.Sync();

  auto source_pipeline = pipeline_service.LoadPipeline(source_id);
  ASSERT_NE(source_pipeline, nullptr);
  ASSERT_NE(source_pipeline->pipeline_, nullptr);
  const auto package = AdjustmentTransferService::Capture(*source_pipeline->pipeline_);
  ASSERT_TRUE(PackageContainsOperator(package, OperatorType::ODT));
  pipeline_service.SavePipeline(source_pipeline);

  const sl_element_id_t ids[] = {target_id};
  const auto result = AdjustmentTransferService::Apply(
      pipeline_service, history_service, std::span<const sl_element_id_t>(ids), package,
      "Pasted Adjustments");
  ASSERT_EQ(result.failures_.size(), 0u);
  ASSERT_EQ(result.applied_ids_.size(), 1u);

  auto target_pipeline = pipeline_service.LoadPipeline(target_id);
  ASSERT_NE(target_pipeline, nullptr);
  ASSERT_NE(target_pipeline->pipeline_, nullptr);
  const auto live_odt =
      OperatorParamsFor(*target_pipeline->pipeline_, PipelineStageName::Output_Transform,
                        OperatorType::ODT);
  EXPECT_EQ(live_odt["odt"]["encoding_space"], "rec2020");
  EXPECT_EQ(live_odt["odt"]["encoding_eotf"], "st2084");
  EXPECT_DOUBLE_EQ(live_odt["odt"]["peak_luminance"].get<double>(), 400.0);
  EXPECT_EQ(live_odt["odt"]["open_drt"]["look_preset"], "umbra");
  EXPECT_EQ(live_odt["odt"]["open_drt"]["tonescale_preset"], "aces_2_0");
  EXPECT_EQ(live_odt["odt"]["open_drt"]["creative_white"], "d60");
  EXPECT_NEAR(live_odt["odt"]["open_drt"]["parameters"]["tn_con"].get<double>(), 1.85, 1e-6);
  pipeline_service.SavePipeline(target_pipeline);

  auto history = history_service.LoadHistory(target_id);
  ASSERT_NE(history, nullptr);
  ASSERT_NE(history->history_, nullptr);
  const auto reconstructed =
      history->history_->ReconstructPipelineParamsForVersion(history->history_->GetActiveVersionID());
  ASSERT_TRUE(reconstructed.has_value());
  CPUPipelineExecutor reconstructed_pipeline;
  reconstructed_pipeline.ImportPipelineParams(*reconstructed);
  const auto reconstructed_odt =
      OperatorParamsFor(reconstructed_pipeline, PipelineStageName::Output_Transform,
                        OperatorType::ODT);
  EXPECT_EQ(reconstructed_odt["odt"]["encoding_space"], "rec2020");
  EXPECT_EQ(reconstructed_odt["odt"]["encoding_eotf"], "st2084");
  EXPECT_DOUBLE_EQ(reconstructed_odt["odt"]["peak_luminance"].get<double>(), 400.0);
  EXPECT_EQ(reconstructed_odt["odt"]["open_drt"]["look_preset"], "umbra");
  history_service.SaveHistory(history);

  pipeline_service.Sync();
  history_service.Sync();

  PipelineMgmtService reopened_pipeline_service(project.GetStorage());
  auto reopened_pipeline = reopened_pipeline_service.LoadPipeline(target_id);
  ASSERT_NE(reopened_pipeline, nullptr);
  ASSERT_NE(reopened_pipeline->pipeline_, nullptr);
  const auto reopened_odt =
      OperatorParamsFor(*reopened_pipeline->pipeline_, PipelineStageName::Output_Transform,
                        OperatorType::ODT);
  EXPECT_EQ(reopened_odt["odt"]["encoding_space"], "rec2020");
  EXPECT_EQ(reopened_odt["odt"]["encoding_eotf"], "st2084");
  EXPECT_DOUBLE_EQ(reopened_odt["odt"]["peak_luminance"].get<double>(), 400.0);
  EXPECT_EQ(reopened_odt["odt"]["open_drt"]["look_preset"], "umbra");
  reopened_pipeline_service.SavePipeline(reopened_pipeline);
}

}  // namespace
}  // namespace alcedo
