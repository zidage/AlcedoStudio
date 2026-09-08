//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/content_key.hpp"
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
  EXPECT_EQ(kProjectFileVersion, "0.6.0");
  EXPECT_EQ(kMinSupportedProjectFileVersion, "0.6.0");
  EXPECT_EQ(kMaxSupportedProjectFileVersion, "0.6.0");
  EXPECT_EQ(kPackedProjectFormatVersion, 6u);
  EXPECT_EQ(kPipelineDocumentFormatVersion, 6u);
  EXPECT_EQ(kImageEditSchemaVersion, 4u);
  EXPECT_EQ(kCommitFormatVersion, 4u);
  EXPECT_EQ(kChainFormatVersion, 4u);
  EXPECT_EQ(kPipelineEditBatchFormatVersion, 3u);
  EXPECT_EQ(kRootStateFormatVersion, 4u);
  EXPECT_EQ(kCheckpointStateFormatVersion, 4u);
  EXPECT_EQ(kMiniGitJournalRecordFormatVersion, 5u);
  EXPECT_EQ(kAdjustmentTransferSchema, "alcedo.adjustment_transfer.v4");
  EXPECT_EQ(kMaskAssetFormatVersion, 1u);
  EXPECT_EQ(kMaskAssetPackedR8FormatId, 1u);
  EXPECT_EQ(kMaximumRasterMaskAxis, 4096u);
  EXPECT_EQ(kBrushSourceFormatVersion, 1u);
  EXPECT_EQ(kBrushRasterAlgorithmVersion, 1u);
  EXPECT_EQ(kProjectMaskCacheFormatVersion, 1u);
  EXPECT_EQ(kBrushDabSpacingRadiusFraction, 0.25f);
  EXPECT_EQ(kMaskImplementationVersion, 3u);
  EXPECT_EQ(static_cast<std::uint8_t>(BrushStrokeMode::Paint), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(BrushStrokeMode::Erase), 1);
}

TEST(BrushSourceFormatBoundary, BrushJsonRoundTripStoresStrokeFieldsAndOmitsAssetKey) {
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddParameterizedBrushMask(
      document, MaskId{"mask.persisted"},
      {grade_mask_test::MakePaintStroke("stroke.1", 8.0f, 12.0f, 4.0f)});
  auto       json   = document.ToJson();
  const auto source = PrimaryGradeJson(json).at("masks").at(0).at("source");
  ASSERT_TRUE(source.is_object());
  EXPECT_EQ(source.at("kind"), "brush");
  EXPECT_FALSE(source.contains("asset_key"));
  EXPECT_FALSE(source.contains("width"));
  EXPECT_FALSE(source.contains("height"));
  EXPECT_FALSE(source.contains("reference_bounds"));
  EXPECT_EQ(source.at("source_format_version"), kBrushSourceFormatVersion);
  EXPECT_EQ(source.at("raster_algorithm_version"), kBrushRasterAlgorithmVersion);
  ASSERT_EQ(source.at("strokes").size(), 1u);
  EXPECT_EQ(source.at("strokes").at(0).at("id"), "stroke.1");
  EXPECT_EQ(json.at("format_version"), kPipelineDocumentFormatVersion);

  const auto restored = PipelineDocument::FromJson(json);
  const auto* mask    = restored.PrimaryGrade()->FindMask(MaskId{"mask.persisted"});
  ASSERT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  ASSERT_NE(brush, nullptr);
  EXPECT_FALSE(brush->asset_key.has_value());
  ASSERT_EQ(brush->strokes.size(), 1u);
  EXPECT_EQ(brush->strokes[0].id, StrokeId{"stroke.1"});
}

TEST(BrushSourceFormatBoundary, MalformedStrokeFieldsOnAssetBrushAreRejected) {
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.brush"}, MaskAssetKey{"asset_01"});
  const auto before = document.ToJson().dump();
  auto json = document.ToJson();
  auto& source = PrimaryGradeJson(json).at("masks").at(0).at("source");
  source["strokes"] = nlohmann::json::array(
      {{{"id", "stroke.1"}, {"mode", "paint"}, {"samples", nlohmann::json::array({1.0, 2.0})}}});
  source["placement_translation"] = nlohmann::json::array({3.0, 4.0});
  source["source_format_version"] = 1;
  source["raster_algorithm_version"] = 1;
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);
  EXPECT_EQ(document.ToJson().dump(), before);
  const auto* brush = std::get_if<BrushMaskSource>(&document.PrimaryGrade()->FindMask(MaskId{"mask.brush"})->source);
  ASSERT_NE(brush, nullptr);
  EXPECT_TRUE(brush->strokes.empty());
  ASSERT_TRUE(brush->asset_key.has_value());
  EXPECT_EQ(*brush->asset_key, MaskAssetKey{"asset_01"});
}

TEST(BrushSourceFormatBoundary, RasterOnlyFormatIsRejectedBeforeCacheCleanup) {
  const auto root = TestRoot("raster_only_rejected");
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  auto json = CreateDefaultPipelineDocument().ToJson();
  PrimaryGradeJson(json)["masks"] = nlohmann::json::array(
      {nlohmann::json{{"id", "mask.brush"},
                      {"display_name", "Brush"},
                      {"enabled", true},
                      {"opacity", 1.0},
                      {"invert", false},
                      {"source",
                       {{"asset_key", "0123456789abcdef0123456789abcdef"},
                        {"feather_radius", 0.0},
                        {"height", 1},
                        {"kind", "brush"},
                        {"reference_bounds", nlohmann::json::array({0.0, 0.0, 1.0, 1.0})},
                        {"width", 1}}},
                      {"color_range", nullptr},
                      {"luminance_range", nullptr}}});
  const auto source_path = root / "pipeline.json";
  const auto cache_path  = root / "0123456789abcdef0123456789abcdef.r8mask";
  {
    std::ofstream stream(source_path, std::ios::binary | std::ios::trunc);
    stream << json.dump();
  }
  {
    std::ofstream stream(cache_path, std::ios::binary | std::ios::trunc);
    stream << "dummy-cache-bytes";
  }
  const auto source_before = ReadFileBytes(source_path);
  const auto cache_before  = ReadFileBytes(cache_path);
  EXPECT_THROW(
      {
        try {
          (void)PipelineDocument::FromJson(nlohmann::json::parse(source_before));
        } catch (const std::runtime_error& error) {
          EXPECT_NE(std::string{error.what()}.find("brush raster-only asset_key encoding is not supported"),
                    std::string::npos);
          throw;
        }
      },
      std::runtime_error);
  EXPECT_EQ(ReadFileBytes(source_path), source_before);
  EXPECT_EQ(ReadFileBytes(cache_path), cache_before);
  EXPECT_TRUE(std::filesystem::exists(cache_path));
  EXPECT_NE(source_before.find("asset_key"), std::string::npos);
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
  EXPECT_EQ(std::size(names), 61u);
  EXPECT_EQ(names[0], "StrokeUndoRedoRestoresCommandOrderWithoutR8");
  EXPECT_EQ(names[5], "RasterOnlyFormatIsRejectedBeforeCacheCleanup");
  EXPECT_EQ(names[60], "PastedBrushUsesTargetProjectStoragePolicy");
}

}  // namespace
}  // namespace alcedo
