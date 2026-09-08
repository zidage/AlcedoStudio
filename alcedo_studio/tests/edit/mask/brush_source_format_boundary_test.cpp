//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace alcedo {
namespace {

auto TestRoot(std::string_view name) -> std::filesystem::path {
  return std::filesystem::path{"build/tmp/brush_source_format_boundary"} / name;
}

auto ReadFileBytes(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

auto PrimaryGradeJson(nlohmann::json& document_json) -> nlohmann::json& {
  for (auto& node : document_json.at("nodes")) {
    if (node.at("id") == "grade.primary") {
      return node;
    }
  }
  throw std::runtime_error("document JSON is missing grade.primary");
}

TEST(BrushSourceFormatBoundary, CurrentHistoryIdentitiesMatchPublishedConstants) {
  EXPECT_EQ(kProjectFileVersion, "0.5.0");
  EXPECT_EQ(kMinSupportedProjectFileVersion, "0.5.0");
  EXPECT_EQ(kMaxSupportedProjectFileVersion, "0.5.0");
  EXPECT_EQ(kPackedProjectFormatVersion, 5u);
  EXPECT_EQ(kPipelineDocumentFormatVersion, 5u);
  EXPECT_EQ(kImageEditSchemaVersion, 3u);
  EXPECT_EQ(kCommitFormatVersion, 3u);
  EXPECT_EQ(kChainFormatVersion, 3u);
  EXPECT_EQ(kPipelineEditBatchFormatVersion, 2u);
  EXPECT_EQ(kRootStateFormatVersion, 3u);
  EXPECT_EQ(kCheckpointStateFormatVersion, 3u);
  EXPECT_EQ(kMiniGitJournalRecordFormatVersion, 4u);
  EXPECT_EQ(kAdjustmentTransferSchema, "alcedo.adjustment_transfer.v3");
  EXPECT_EQ(kMaskAssetFormatVersion, 1u);
  EXPECT_EQ(kMaskAssetPackedR8FormatId, 1u);
  EXPECT_EQ(kMaximumRasterMaskAxis, 4096u);
  EXPECT_EQ(kBrushSourceFormatVersion, 1u);
  EXPECT_EQ(kBrushRasterAlgorithmVersion, 1u);
  EXPECT_EQ(kProjectMaskCacheFormatVersion, 1u);
  EXPECT_EQ(kBrushDabSpacingRadiusFraction, 0.25f);
  EXPECT_NE(kPipelineDocumentFormatVersion, 6u);
  EXPECT_EQ(static_cast<std::uint8_t>(BrushStrokeMode::Paint), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(BrushStrokeMode::Erase), 1);
}

TEST(BrushSourceFormatBoundary, BrushJsonRoundTripStoresAssetKeyAndOmitsStrokeFields) {
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.persisted"}, MaskAssetKey{"asset_01"});
  auto       json   = document.ToJson();
  const auto source = PrimaryGradeJson(json).at("masks").at(0).at("source");
  ASSERT_TRUE(source.is_object());
  EXPECT_EQ(source.at("kind"), "brush");
  EXPECT_EQ(source.at("asset_key"), "asset_01");
  EXPECT_FALSE(source.contains("strokes"));
  EXPECT_FALSE(source.contains("placement_translation"));
  EXPECT_FALSE(source.contains("source_format_version"));
  EXPECT_FALSE(source.contains("raster_algorithm_version"));
  EXPECT_EQ(json.at("format_version"), kPipelineDocumentFormatVersion);

  const auto restored = PipelineDocument::FromJson(json);
  const auto* mask    = restored.PrimaryGrade()->FindMask(MaskId{"mask.persisted"});
  ASSERT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->asset_key.has_value());
  EXPECT_EQ(*brush->asset_key, MaskAssetKey{"asset_01"});
}

TEST(BrushSourceFormatBoundary, CurrentBrushLoaderIgnoresStrokeFieldsAndKeepsAssetKey) {
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.brush"}, MaskAssetKey{"asset_01"});
  auto json = document.ToJson();
  auto& source = PrimaryGradeJson(json).at("masks").at(0).at("source");
  source["strokes"] = nlohmann::json::array(
      {{{"id", "stroke.1"}, {"mode", "paint"}, {"samples", nlohmann::json::array({1.0, 2.0})}}});
  source["placement_translation"] = nlohmann::json::array({3.0, 4.0});
  source["source_format_version"] = 1;
  const auto restored = PipelineDocument::FromJson(json);
  const auto* mask    = restored.PrimaryGrade()->FindMask(MaskId{"mask.brush"});
  ASSERT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  ASSERT_NE(brush, nullptr);
  ASSERT_TRUE(brush->asset_key.has_value());
  EXPECT_EQ(*brush->asset_key, MaskAssetKey{"asset_01"});
  const auto rewritten = MaskModelToJson(*mask)["source"];
  EXPECT_FALSE(rewritten.contains("strokes"));
  EXPECT_EQ(rewritten.at("asset_key"), "asset_01");
}

TEST(BrushSourceFormatBoundary, UnsupportedPipelineDocumentFormatLeavesSourceFileUnchanged) {
  const auto root = TestRoot("unsupported_document");
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  auto json = CreateDefaultPipelineDocument().ToJson();
  json["format_version"] = 4;
  const auto path        = root / "pipeline.json";
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << json.dump();
  }
  const auto before = ReadFileBytes(path);
  EXPECT_THROW((void)PipelineDocument::FromJson(nlohmann::json::parse(before)), std::runtime_error);
  EXPECT_EQ(ReadFileBytes(path), before);
  EXPECT_NE(before.find("\"format_version\":4"), std::string::npos);
}

TEST(BrushSourceFormatBoundary, UnknownBrushSourceKindIsRejectedWithoutDocumentMutation) {
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = document.ToJson().dump();
  auto json = document.ToJson();
  PrimaryGradeJson(json)["masks"] = nlohmann::json::array(
      {nlohmann::json{{"id", "mask.bad"},
                      {"display_name", ""},
                      {"enabled", true},
                      {"opacity", 1.0},
                      {"invert", false},
                      {"source", {{"kind", "vector_path"}}},
                      {"color_range", nullptr},
                      {"luminance_range", nullptr}}});
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);
  EXPECT_EQ(document.ToJson().dump(), before);
}

TEST(BrushSourceFormatBoundary, HeldMaskStoreReaderKeepsImmutablePixelsAfterHostCacheEviction) {
  const auto root = TestRoot("reader_lifetime");
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  MaskAssetDescriptor descriptor;
  descriptor.extent           = {4, 2};
  descriptor.reference_bounds = CanonicalBrushReferenceBounds();
  const std::vector<std::uint8_t> first_pixels(8, 17);
  const std::vector<std::uint8_t> second_pixels(8, 19);
  MaskStore store(root, 8);
  const auto first_key  = store.Put(descriptor, first_pixels);
  const auto first_held = store.Load(first_key);
  ASSERT_NE(first_held, nullptr);
  EXPECT_EQ(first_held.get(), store.Load(first_key).get());
  const auto* first_data = first_held->pixels.data();
  const auto second_key  = store.Put(descriptor, second_pixels);
  EXPECT_NE(first_key, second_key);
  EXPECT_EQ(store.HostCacheEntryCount(), 1u);
  EXPECT_EQ(first_held->pixels, first_pixels);
  EXPECT_EQ(first_held->pixels.data(), first_data);
  const auto reloaded = store.Load(first_key);
  EXPECT_EQ(reloaded->pixels, first_pixels);
  EXPECT_NE(reloaded.get(), first_held.get());
}

TEST(BrushSourceFormatBoundary, OrdinaryAdjustmentPathStillRejectsMaskTargets) {
  EditorParameterTarget target;
  target.owner_kind = EditorParameterOwnerKind::ColorGradeMask;
  target.node_id    = NodeId{"grade.primary"};
  target.mask_id    = "mask.1";
  target.field_key  = "opacity";
  EXPECT_EQ(DescribeEditorParameterTargetError(target, "opacity"),
            "Mask parameter targets are rejected until NM3");
}

TEST(BrushSourceFormatBoundary, BrushDabCoverageQuantizesWithRoundHalfUpAndHardEdge) {
  EXPECT_EQ(QuantizeMaskCoverageToR8(0.0f), 0);
  EXPECT_EQ(QuantizeMaskCoverageToR8(1.0f), 255);
  EXPECT_EQ(QuantizeMaskCoverageToR8(0.5f), 128);
  EXPECT_THROW((void)QuantizeMaskCoverageToR8(std::numeric_limits<float>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_FLOAT_EQ(BrushDabCoverage(0.0f, 10.0f, 1.0f, 1.0f), 1.0f);
  EXPECT_FLOAT_EQ(BrushDabCoverage(10.0f, 10.0f, 0.5f, 1.0f), 0.5f);
  EXPECT_FLOAT_EQ(BrushDabCoverage(10.01f, 10.0f, 1.0f, 1.0f), 0.0f);
  EXPECT_FLOAT_EQ(BrushDabCoverage(5.0f, 10.0f, 1.0f, 0.5f), 1.0f);
  EXPECT_FLOAT_EQ(BrushDabCoverage(7.5f, 10.0f, 1.0f, 0.5f), 0.5f);
  EXPECT_FLOAT_EQ(BrushDabCoverage(10.0f, 10.0f, 1.0f, 0.5f), 0.0f);
  EXPECT_EQ(PaintBrushR8(30, 200), 200);
  EXPECT_EQ(PaintBrushR8(90, 200), 200);
  EXPECT_EQ(EraseBrushR8(200, 255), 0);
  EXPECT_EQ(EraseBrushR8(200, 0), 200);
  BrushCanonicalSample invalid;
  invalid.radius = 0.0f;
  EXPECT_THROW(ValidateBrushCanonicalSample(invalid), std::invalid_argument);
  EXPECT_THROW((void)BrushDabCoverage(-1.0f, 10.0f, 1.0f, 1.0f), std::invalid_argument);
}

TEST(BrushSourceFormatBoundary, CanonicalBrushRasterExtentCapsLongEdgeAt4096) {
  EXPECT_EQ(CanonicalBrushRasterExtent({800, 600}), (Extent2D{800, 600}));
  EXPECT_EQ(CanonicalBrushRasterExtent({8000, 4000}), (Extent2D{4096, 2048}));
  EXPECT_EQ(CanonicalBrushRasterExtent({4097, 1}), (Extent2D{4096, 1}));
  EXPECT_TRUE(CanonicalBrushReferenceBounds().IsFullFrame());
  EXPECT_THROW((void)CanonicalBrushRasterExtent({}), std::invalid_argument);
  const auto center = CanonicalBrushTexelReferenceCenter(0, 0, {4096, 2048}, {8000, 4000});
  EXPECT_NEAR(center.x, 0.5f * 8000.0f / 4096.0f, 1.0e-5f);
  EXPECT_NEAR(center.y, 0.5f * 4000.0f / 2048.0f, 1.0e-5f);
}

TEST(BrushSourceFormatBoundary, BrushFeatherRadiusMatchesNativeTexelScale) {
  const Extent2D     raster{64, 32};
  const Extent2D     full{64, 32};
  const auto         bounds = CanonicalBrushReferenceBounds();
  EXPECT_FLOAT_EQ(BrushFeatherRadiusToSourceTexels(10.0f, raster, bounds, full), 10.0f);
  NormalizedRect half = bounds;
  half.w              = 0.5f;
  EXPECT_NEAR(BrushFeatherRadiusToSourceTexels(10.0f, raster, half, full), 15.0f, 1.0e-5f);
}

TEST(BrushSourceFormatBoundary, PackedR8BilinearSampleMatchesNativeCenterFilter) {
  const std::vector<std::uint8_t> pixels{0, 255};
  EXPECT_FLOAT_EQ(SamplePackedR8Bilinear(pixels, {2, 1}, 0.5f, 0.5f), 0.5f);
  EXPECT_FLOAT_EQ(SamplePackedR8Bilinear(pixels, {2, 1}, -0.1f, 0.5f), 0.0f);
}

TEST(BrushSourceFormatBoundary, PlannedParameterizedBrushTestsAreCatalogued) {
  constexpr std::string_view names[] = {
      "BrushSourceRoundTripsWithoutRasterFiles",
      "BrushAppendKeepsExistingStrokeIds",
      "InvalidStrokeDoesNotPartiallyMutateSource",
      "BrushTranslationDoesNotCopyOrRewriteSamples",
      "StrokeUndoRedoRestoresCommandOrderWithoutR8",
      "FirstBrushStrokeCreatesOneCommit",
      "BrushMoveUndoRestoresExactTranslation",
      "AppendHistoryDoesNotRepeatEarlierSamples",
      "ParameterizedBrushSurvivesWalRecoveryAndPaste",
      "RasterOnlyFormatIsRejectedBeforeCacheCleanup",
      "RegionalBrushReplayMatchesFullEvaluation",
      "EraseUndoRestoresEarlierPaint",
      "RemovingUnionMaximumPreservesOtherMasks",
      "BrushEventGroupingPreservesCanonicalPixels",
      "FeatherRebuildMatchesCompleteDistanceEvaluation",
      "MaskReferenceMappingRoundTripsAcrossZoomPanAndDpr",
      "BrushMoveThenDrawUsesTranslatedLocalCoordinates",
      "RepeatedBrushMoveDoesNotBlurCoverage",
      "DetailPatchDoesNotChangeMaskReferenceSpace",
      "InvalidGeometryRejectsMaskPress",
      "ExistingMaskEditHasControlsAndNoCoverageFill",
      "MaskControlsUpdateWhileRenderIsHeld",
      "MaskOverlayReusesNodesForMovement",
      "MaskControlsRecreateAfterSceneInvalidation",
      "MaskControlsKeepLogicalSizeAndThemeColors",
      "ExistingRadialMoveUpdatesInteractivePixelsBeforeRelease",
      "ExistingGradientMovePreservesDirectionAndUpdatesInteractivePixels",
      "RadialFeatherControlsMatchEvaluator",
      "DegenerateAnalyticCreationCreatesNoCommit",
      "EscapeRestoresAnalyticSourceWithoutCommit",
      "MultipleStrokesUseOneBrushMask",
      "SizeAndStrengthChangesPersistInStrokeSamples",
      "MovingExistingBrushUpdatesInteractivePixels",
      "BrushMoveDoesNotAppendStroke",
      "UndoLastStrokePreservesEarlierStrokes",
      "BrushReleaseCreatesNoHistoricalRasterFile",
      "MaskMoveNeverMutatesSourceDuringRender",
      "LatestMoveValueSurvivesRelease",
      "CurrentGradeCoverageHasOneRetainedResult",
      "DelayedOldFrameCannotReplaceNewMaskPosition",
      "QualityMaskEvaluationDoesNotOverwriteInteractiveCache",
      "RebuiltMaskMatchesCacheHitPixels",
      "ProjectMaskCacheSettingsSurviveSaveAndReopen",
      "ThousandStrokesKeepOneRasterSlot",
      "CacheClearCannotDeleteAnotherProjectsFiles",
      "OldWriterCannotRecreateClearedCache",
      "RootChangeFailurePreservesOldSetting",
      "CacheWriteFailureDoesNotLoseStrokeHistory",
      "CloseCleanupRunsAfterParameterSaveAndReadersFinish",
      "MaskModePreservesSixTabs",
      "MaskSelectionDoesNotSubmitOrJumpScroll",
      "KeyboardMaskMoveUsesSameInteractiveRoute",
      "CacheSettingsTargetSelectedProjectOnly",
      "ProjectClearExplainsThatBrushHistoryIsRetained",
      "GrabCancellationDoesNotCommitBrush",
      "DuplicateReleaseCommitsOnce",
      "VersionCheckoutRejectsOldMaskFrame",
      "ProjectSwitchRejectsOldCacheWrite",
      "MaskDeleteWithViewerFocusDoesNotDeleteGrade",
      "WorkspaceHideRestoresUnfinishedMaskMove",
      "CachelessProjectRestoresAllMaskVersions",
      "NativeMaskReplayMatchesExpectedCoverage",
      "RepeatedHistoryNavigationKeepsRasterFileCountBounded",
      "InterruptedCacheReplaceLeavesNoPartialFile",
      "PastedBrushUsesTargetProjectStoragePolicy",
  };
  EXPECT_EQ(std::size(names), 65u);
  EXPECT_EQ(names[0], "BrushSourceRoundTripsWithoutRasterFiles");
  EXPECT_EQ(names[9], "RasterOnlyFormatIsRejectedBeforeCacheCleanup");
  EXPECT_EQ(names[64], "PastedBrushUsesTargetProjectStoragePolicy");
}

}  // namespace
}  // namespace alcedo
