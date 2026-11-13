//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_document_checkpoint.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/project_package_backend.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_clock_test_access.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "test_camera_profile.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

auto LoadExpectedBytes(const std::string& name) -> std::string {
  const auto path = std::filesystem::path(PIPELINE_HISTORY_FORMAT_EXPECTED_SERIALIZED_DIR) / name;
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input) << path.string();
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

/// Loads the neutral color wheel JSON of persisted format version 1 (ACEScc CDL, before the tone
/// wheels) into the default Color Grade of @p document, as documents written before the tone
/// wheels hold it. The Model writes this JSON back unchanged until a wheel changes.
void LoadVersionOneNeutralColorWheels(PipelineDocument& document) {
  auto* wheels = dynamic_cast<ColorWheelModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::ColorWheel()));
  ASSERT_NE(wheels, nullptr);
  const auto neutral = [](double base) {
    return nlohmann::json{{"color_offset", {{"x", base}, {"y", base}, {"z", base}}},
                          {"disc", {{"x", 0.0}, {"y", 0.0}}},
                          {"luminance_offset", 0.0},
                          {"strength", 1.0}};
  };
  wheels->LoadJson({{"gain", neutral(1.0)}, {"gamma", neutral(1.0)}, {"lift", neutral(0.0)}});
  EXPECT_TRUE(wheels->IsDefault());
}

/// Sets the RAW import look of the code before #891 on the default Color Grade of @p document:
/// contrast 15 and the identity RGB curve (the Film-like curve replaced it). The stored files of
/// this test were written with that look.
void ApplyImportLookBeforeFilmLike(PipelineDocument& document) {
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->FindAdjustmentByType(type_ids::Contrast())
      ->LoadJson({{"contrast", kImportLookContrastBeforeFilmLike}});
  auto* curve = dynamic_cast<CurveModel*>(grade->FindAdjustmentByType(type_ids::Curve()));
  ASSERT_NE(curve, nullptr);
  curve->SetPoints(IdentityCurvePoints());
}

auto MultiGradeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  ApplyImportLookBeforeFilmLike(document);
  EXPECT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
  grade_mask_test::AddRadialMask(document, MaskId{"mask.radial"});
  return document;
}

/// One document of the Film Grain compatibility files: its root document, the history entries from
/// the root to the head, and the commits of those entries at fixed timestamps.
struct FilmGrainHistoryCase {
  std::uint32_t                  element_id = 0;
  nlohmann::json                 raw_json;
  PipelineDocument               root;
  PipelineDocument               head;
  std::vector<PipelineEditBatch> batches;
};

/// Applies @p patch to @p field of @p document as one editor write and records its history entry.
void RecordParameterEntry(PipelineDocument& document, std::string_view field,
                          const nlohmann::json& patch, std::vector<PipelineEditBatch>& batches) {
  std::string error;
  const auto  target = CompleteCurrentPanelParameterTarget(document, std::string{field}, &error);
  ASSERT_TRUE(target.has_value()) << error;
  nlohmann::json before;
  ASSERT_TRUE(ReadEditorParameterJson(document, *target, &before, &error)) << error;
  ASSERT_TRUE(ApplyEditorParameterPatch(document, *target, patch, &error)) << error;
  nlohmann::json after;
  ASSERT_TRUE(ReadEditorParameterJson(document, *target, &after, &error)) << error;
  batches.push_back(MakeSetParameterBatch(*target, before, after, true, true, "DRT"));
}

/// The two documents of plan 'film_grain_format_and_stock_plan.md', section 2.4. `film_print`: the
/// film print of Kodak 5207 on 2383, then Film Grain 0 to 0.6 and Halation 0.6 to 0.4. `open_drt`:
/// OpenDRT, then Film Grain 0 to 0.3.
auto MakeFilmGrainHistoryCase(std::string_view name) -> FilmGrainHistoryCase {
  FilmGrainHistoryCase result;
  result.raw_json = nlohmann::json{{"CameraModel", "RootStateCamera"}};
  auto document   = CreateDefaultPipelineDocument();
  ApplyImportLookBeforeFilmLike(document);
  gpu_dag_test::EnsureTestCameraProfile(document);
  std::string error;
  if (name == "film_print") {
    result.element_id = 42;
    const auto odt    = CompleteCurrentPanelParameterTarget(document, "odt", &error);
    EXPECT_TRUE(odt.has_value()) << error;
    EXPECT_TRUE(ApplyEditorParameterPatch(document, *odt,
                                          {{"method", "film_print"},
                                           {"film_print",
                                            {{"negative", "kodak_5207"},
                                             {"print_material", "kodak_2383"},
                                             {"scene_light", "reference"}}}},
                                          &error))
        << error;
  } else {
    result.element_id = 43;
  }
  result.root = ClonePipelineDocument(document);
  if (name == "film_print") {
    RecordParameterEntry(document, "film_grain", {{"strength", 0.6}}, result.batches);
    RecordParameterEntry(document, "odt", {{"film_print", {{"halation", 0.4}}}}, result.batches);
  } else {
    RecordParameterEntry(document, "film_grain", {{"strength", 0.3}}, result.batches);
  }
  result.head = std::move(document);
  return result;
}

/// The commits of @p history_case, oldest first, at the timestamps 1000, 2000, and so on.
auto FilmGrainHistoryCommits(const FilmGrainHistoryCase& history_case, const root_id_t& root_id)
    -> std::vector<EditCommit> {
  std::vector<EditCommit> commits;
  head_commit_hash_t      parent;
  std::uint64_t           created_at_ns = 1000;
  for (const auto& batch : history_case.batches) {
    commits.push_back(edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
        root_id, parent, created_at_ns, batch));
    parent = commits.back().GetCommitHash();
    created_at_ns += 1000;
  }
  return commits;
}

auto FilmGrainHistoryChainHash(const root_id_t& root_id, const std::vector<EditCommit>& commits)
    -> transaction_chain_hash_t {
  auto chain = ComputeRootChainHash(root_id);
  for (const auto& commit : commits) {
    chain = FoldTransactionChainHash(chain, commit.GetCommitHash());
  }
  return chain;
}

}  // namespace

TEST(PipelineHistoryFormatTable, PublishedConstantsIdentifyTheDocumentHistoryCutover) {
  EXPECT_EQ(kProjectFileVersion, "0.11.0");
  EXPECT_EQ(kMinSupportedProjectFileVersion, "0.10.0");
  EXPECT_EQ(kMaxSupportedProjectFileVersion, kProjectFileVersion);
  EXPECT_EQ(kPackedProjectFormatVersion, 7u);
  EXPECT_EQ(kPipelineDocumentFormatVersion, 7u);
  EXPECT_EQ(kImageEditSchemaVersion, 5u);
  EXPECT_EQ(kCommitFormatVersion, 5u);
  EXPECT_EQ(kChainFormatVersion, 5u);
  EXPECT_EQ(kPipelineEditBatchFormatVersion, 4u);
  EXPECT_EQ(kRootStateFormatVersion, 5u);
  EXPECT_EQ(kCheckpointStateFormatVersion, 5u);
  EXPECT_EQ(kMiniGitJournalRecordFormatVersion, 7u);
  EXPECT_EQ(kAdjustmentTransferSchema, "alcedo.adjustment_transfer.v7");
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported(kProjectFileVersion));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.8.0"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.7.0"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.5.0"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.3.0"));
}

TEST(PipelineHistoryFormatTable, ProjectMetadataExpectedSerializedMatchesPublishedVersion) {
  const auto expected_serialized = nlohmann::json::parse(LoadExpectedBytes("project_metadata.json"));
  EXPECT_EQ(expected_serialized.at("project_file_version").get<std::string>(),
            std::string(kProjectFileVersion));
  EXPECT_EQ(expected_serialized.at("project_file_min_supported_version").get<std::string>(),
            std::string(kMinSupportedProjectFileVersion));
  EXPECT_EQ(expected_serialized.at("project_file_max_supported_version").get<std::string>(),
            std::string(kMaxSupportedProjectFileVersion));
}

TEST(PipelineDocumentCheckpointFormat, FullDocumentExpectedSerializedWithGradesAndMasksRemainsStable) {
  const auto document = MultiGradeDocument();
  EXPECT_EQ(document.ToJson().dump(), LoadExpectedBytes("pipeline_document_multi_grade.json"));
}

TEST(PipelineDocumentCheckpointFormat, RootExpectedSerializedBindsOwnerDocumentAndDevelopIdentity) {
  const auto document = MultiGradeDocument();
  const auto encoded =
      EncodePipelineRootState(42, document, nlohmann::json{{"CameraModel", "RootStateCamera"}});
  EXPECT_EQ(encoded.dump(), LoadExpectedBytes("pipeline_root.json"));
  const auto decoded = DecodePipelineRootState(encoded);
  EXPECT_EQ(decoded.element_id, 42u);
  EXPECT_EQ(ComputeRootId(42, decoded.document, decoded.raw_color_context),
            ComputeRootId(42, document, nlohmann::json{{"CameraModel", "RootStateCamera"}}));
}

TEST(PipelineDocumentCheckpointFormat, CheckpointExpectedSerializedCarriesRootHeadChainAndDocument) {
  const auto document = MultiGradeDocument();
  const auto root_id  = ComputeRootId(7, document, std::nullopt);
  const auto chain    = ComputeRootChainHash(root_id);
  const auto encoded  = EncodePipelineDocumentCheckpoint(root_id, std::nullopt, chain, document);
  EXPECT_EQ(encoded.dump(), LoadExpectedBytes("pipeline_checkpoint.json"));
  const auto decoded = DecodePipelineDocumentCheckpoint(encoded);
  EXPECT_EQ(decoded.root_id, root_id);
  EXPECT_FALSE(decoded.head_commit_hash.has_value());
  EXPECT_EQ(decoded.transaction_chain_hash, chain);
}

TEST(PipelineDocumentCheckpointFormat,
     ExistingRawRootDocumentsSerializeByteIdenticalAfterRasterChange) {
  // raw_root_before_raster_input.json and raw_checkpoint_before_raster_input.json were written by
  // the document code of main (abdb000c8) before raster input existed: the default document with
  // a bound camera profile, element 42. The same RAW document must encode to the same bytes, keep
  // its root id, and never gain the Develop "input" key. Its color wheels are version 1 JSON.
  auto document = CreateDefaultPipelineDocument();
  ApplyImportLookBeforeFilmLike(document);
  gpu_dag_test::EnsureTestCameraProfile(document);
  LoadVersionOneNeutralColorWheels(document);
  const auto raw_json   = nlohmann::json{{"CameraModel", "RootStateCamera"}};
  const auto root_bytes = LoadExpectedBytes("raw_root_before_raster_input.json");
  EXPECT_EQ(EncodePipelineRootState(42, document, raw_json).dump(), root_bytes);
  const auto root = DecodePipelineRootState(nlohmann::json::parse(root_bytes));
  EXPECT_EQ(EncodePipelineRootState(root.element_id, root.document, root.raw_color_context).dump(),
            root_bytes);
  ASSERT_NE(root.document.Develop(), nullptr);
  EXPECT_FALSE(root.document.Develop()->Params().RasterInput().has_value());
  EXPECT_FALSE(root.document.Develop()->Params().ToJson().contains("input"));

  const auto checkpoint_bytes = LoadExpectedBytes("raw_checkpoint_before_raster_input.json");
  const auto checkpoint = DecodePipelineDocumentCheckpoint(nlohmann::json::parse(checkpoint_bytes));
  // The stored root id was computed by main.
  EXPECT_EQ(ComputeRootId(42, document, raw_json), checkpoint.root_id);
  EXPECT_EQ(ComputeRootId(42, root.document, root.raw_color_context), checkpoint.root_id);
  EXPECT_EQ(EncodePipelineDocumentCheckpoint(checkpoint.root_id, checkpoint.head_commit_hash,
                                             checkpoint.transaction_chain_hash, checkpoint.document)
                .dump(),
            checkpoint_bytes);

  // A Develop payload with an imported camera profile, as main wrote it.
  std::ifstream develop_in(std::filesystem::path(ALCEDO_EXPECTED_JSON_DIR) /
                           "imported_camera_profile_raw_expected_develop.json");
  ASSERT_TRUE(develop_in.good());
  const auto         develop_json = nlohmann::json::parse(develop_in);
  DevelopParamsModel develop;
  develop.LoadJson(develop_json);
  EXPECT_EQ(develop.ToJson().dump(), develop_json.dump());
}

TEST(PipelineDocumentCheckpointFormat,
     ExistingCurveRootDocumentsSerializeByteIdenticalAfterChannelCurves) {
  // curve_root_before_channel_curves.json is raw_root_before_raster_input.json (the default
  // document with a bound camera profile, element 42) with a four-point RGB curve, in the Curve
  // JSON of the code before the red, green, and blue curves: {"points": [...]} only. The same
  // document must encode to the same bytes (so it keeps its root id), read identity channel
  // curves, and never gain the channel keys (tone_curve_editor_plan.md, TC2).
  auto document = CreateDefaultPipelineDocument();
  ApplyImportLookBeforeFilmLike(document);
  gpu_dag_test::EnsureTestCameraProfile(document);
  LoadVersionOneNeutralColorWheels(document);
  auto* curve =
      dynamic_cast<CurveModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Curve()));
  ASSERT_NE(curve, nullptr);
  const std::vector<CurvePoint> rgb{{0.0f, 0.0f}, {0.25f, 0.18f}, {0.6f, 0.7f}, {1.0f, 1.0f}};
  curve->SetPoints(rgb);
  const auto raw_json   = nlohmann::json{{"CameraModel", "RootStateCamera"}};
  const auto root_bytes = LoadExpectedBytes("curve_root_before_channel_curves.json");
  // The stored root has the HLS defaults of its time; the default document has others (#553).
  // Load the stored HLS params so that this test compares the curve only.
  const auto stored_grade =
      nlohmann::json::parse(root_bytes).at("pipeline_document").at("nodes").at(1);
  ASSERT_EQ(stored_grade.at("id"), "grade.primary");
  for (const auto& adjustment : stored_grade.at("adjustments")) {
    if (adjustment.at("id") == "grade.primary.hls") {
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Hls())->LoadJson(
          adjustment.at("params"));
    }
  }
  EXPECT_EQ(EncodePipelineRootState(42, document, raw_json).dump(), root_bytes);

  const auto root = DecodePipelineRootState(nlohmann::json::parse(root_bytes));
  EXPECT_EQ(EncodePipelineRootState(root.element_id, root.document, root.raw_color_context).dump(),
            root_bytes);
  EXPECT_EQ(ComputeRootId(42, root.document, root.raw_color_context),
            ComputeRootId(42, document, raw_json));
  const auto* restored = dynamic_cast<const CurveModel*>(
      root.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Curve()));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->Points(), rgb);
  for (const auto channel : {CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue}) {
    EXPECT_EQ(restored->ChannelPoints(channel), IdentityCurvePoints()) << CurveChannelKey(channel);
    EXPECT_FALSE(restored->ToJson().contains(std::string{CurveChannelKey(channel)}));
  }
}

TEST(PipelineDocumentCheckpointFormat,
     ExistingLmtRootDocumentsSerializeByteIdenticalAfterEncodingChange) {
  // lmt_root_before_lut_encoding.json and lmt_checkpoint_before_lut_encoding.json were written by
  // the document code of 8f8e71220, before LMT input and output encodings existed: the default
  // document with a bound camera profile, element 42, and a library LUT at strength 0.35. The
  // same document must encode to the same bytes, keep its root id, read both encodings as
  // ACEScc, and never gain the encoding keys (lut_color_encoding_plan.md, section 6.1). Its color
  // wheels are version 1 JSON.
  auto document = CreateDefaultPipelineDocument();
  ApplyImportLookBeforeFilmLike(document);
  gpu_dag_test::EnsureTestCameraProfile(document);
  LoadVersionOneNeutralColorWheels(document);
  auto* lmt =
      dynamic_cast<LmtModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetReference(LibraryLutReference{"user/films/look.cube"}, "Look");
  lmt->SetStrength(0.35f);
  const auto raw_json   = nlohmann::json{{"CameraModel", "RootStateCamera"}};
  const auto root_bytes = LoadExpectedBytes("lmt_root_before_lut_encoding.json");
  EXPECT_EQ(EncodePipelineRootState(42, document, raw_json).dump(), root_bytes);
  const auto root = DecodePipelineRootState(nlohmann::json::parse(root_bytes));
  EXPECT_EQ(EncodePipelineRootState(root.element_id, root.document, root.raw_color_context).dump(),
            root_bytes);
  const auto* restored = dynamic_cast<const LmtModel*>(
      root.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->InputEncoding(), "acescc");
  EXPECT_EQ(restored->OutputEncoding(), "acescc");
  EXPECT_FALSE(restored->ToJson().contains("input_encoding"));
  EXPECT_FALSE(restored->ToJson().contains("output_encoding"));

  const auto checkpoint_bytes = LoadExpectedBytes("lmt_checkpoint_before_lut_encoding.json");
  const auto checkpoint = DecodePipelineDocumentCheckpoint(nlohmann::json::parse(checkpoint_bytes));
  EXPECT_EQ(ComputeRootId(42, document, raw_json), checkpoint.root_id);
  EXPECT_EQ(ComputeRootId(42, root.document, root.raw_color_context), checkpoint.root_id);
  EXPECT_EQ(EncodePipelineDocumentCheckpoint(checkpoint.root_id, checkpoint.head_commit_hash,
                                             checkpoint.transaction_chain_hash, checkpoint.document)
                .dump(),
            checkpoint_bytes);
}

TEST(PipelineDocumentCheckpointFormat,
     ExistingFilmGrainRootDocumentsSerializeByteIdenticalAfterFormatChange) {
  // The drt_film_grain_*_before_film_format.json files were written by the document code of
  // 6b8eebaf6, before the film format existed (plan 'film_grain_format_and_stock_plan.md', section
  // 2.4). Each document must encode to the same bytes, keep its root id, read the format 35mm, and
  // never gain the key `film_format`.
  const auto roots =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_root_before_film_format.json"));
  const auto checkpoints =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_checkpoint_before_film_format.json"));
  const auto histories =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_history_before_film_format.json"));
  for (const char* name : {"film_print", "open_drt"}) {
    SCOPED_TRACE(name);
    const auto history_case = MakeFilmGrainHistoryCase(name);
    const auto root_bytes   = roots.at(name).dump();
    EXPECT_EQ(
        EncodePipelineRootState(history_case.element_id, history_case.root, history_case.raw_json)
            .dump(),
        root_bytes);
    const auto root = DecodePipelineRootState(roots.at(name));
    EXPECT_EQ(
        EncodePipelineRootState(root.element_id, root.document, root.raw_color_context).dump(),
        root_bytes);
    ASSERT_NE(root.document.Drt(), nullptr);
    EXPECT_EQ(root.document.Drt()->Params().FilmFormat(), FilmFormat::Film35mm);
    EXPECT_FALSE(root.document.Drt()->Params().ToJson().contains("film_format"));

    const auto checkpoint_bytes = checkpoints.at(name).dump();
    const auto checkpoint       = DecodePipelineDocumentCheckpoint(checkpoints.at(name));
    const auto root_id = ComputeRootId(root.element_id, root.document, root.raw_color_context);
    EXPECT_EQ(root_id, checkpoint.root_id);
    EXPECT_EQ(ComputeRootId(history_case.element_id, history_case.root, history_case.raw_json),
              checkpoint.root_id);
    EXPECT_EQ(
        EncodePipelineDocumentCheckpoint(checkpoint.root_id, checkpoint.head_commit_hash,
                                         checkpoint.transaction_chain_hash, checkpoint.document)
            .dump(),
        checkpoint_bytes);
    EXPECT_EQ(EncodePipelineDocumentCheckpoint(checkpoint.root_id, checkpoint.head_commit_hash,
                                               checkpoint.transaction_chain_hash, history_case.head)
                  .dump(),
              checkpoint_bytes);
    EXPECT_FALSE(checkpoint.document.Drt()->Params().ToJson().contains("film_format"));

    // The stored entries give the head and the chain of the checkpoint.
    ASSERT_EQ(histories.at(name).size(), history_case.batches.size());
    for (std::size_t index = 0; index < history_case.batches.size(); ++index) {
      EXPECT_EQ(history_case.batches[index].CanonicalJSON().dump(),
                histories.at(name).at(index).dump());
    }
    const auto commits = FilmGrainHistoryCommits(history_case, root_id);
    EXPECT_EQ(head_commit_hash_t{commits.back().GetCommitHash()}, checkpoint.head_commit_hash);
    EXPECT_EQ(FilmGrainHistoryChainHash(root_id, commits), checkpoint.transaction_chain_hash);
  }
}

TEST(PipelineDocumentCheckpointFormat, ExistingFilmGrainHistoryReplaysAfterFormatChange) {
  // Each stored entry must apply to the stored root: its before value equals the current Model
  // JSON. Replay gives the checkpoint document, and undo and redo give the root and the head.
  const auto roots =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_root_before_film_format.json"));
  const auto checkpoints =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_checkpoint_before_film_format.json"));
  const auto histories =
      nlohmann::json::parse(LoadExpectedBytes("drt_film_grain_history_before_film_format.json"));
  for (const char* name : {"film_print", "open_drt"}) {
    SCOPED_TRACE(name);
    const auto root       = DecodePipelineRootState(roots.at(name));
    const auto checkpoint = DecodePipelineDocumentCheckpoint(checkpoints.at(name));
    const auto root_json  = root.document.ToJson().dump();
    const auto head_json  = checkpoint.document.ToJson().dump();
    std::vector<PipelineEditBatch> batches;
    for (const auto& batch_json : histories.at(name)) {
      batches.push_back(PipelineEditBatch::FromJSON(batch_json));
    }
    FilmGrainHistoryCase history_case;
    history_case.batches = batches;
    std::string error;
    const auto  replayed = ReplayPipelineDocumentFromRoot(
        root.document, FilmGrainHistoryCommits(history_case, checkpoint.root_id), &error);
    ASSERT_TRUE(replayed.has_value()) << error;
    EXPECT_EQ(replayed->ToJson().dump(), head_json);

    auto document = ClonePipelineDocument(root.document);
    for (const auto& batch : batches) {
      ASSERT_TRUE(
          ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error))
          << error;
    }
    EXPECT_EQ(document.ToJson().dump(), head_json);
    for (auto batch = batches.rbegin(); batch != batches.rend(); ++batch) {
      ASSERT_TRUE(
          ApplyPipelineEditBatch(document, *batch, PipelineEditApplyDirection::Inverse, &error))
          << error;
    }
    EXPECT_EQ(document.ToJson().dump(), root_json);
    for (const auto& batch : batches) {
      ASSERT_TRUE(
          ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error))
          << error;
    }
    EXPECT_EQ(document.ToJson().dump(), head_json);
    const auto* grain = dynamic_cast<const FilmGrainModel*>(
        document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()));
    ASSERT_NE(grain, nullptr);
    EXPECT_FLOAT_EQ(grain->Value(), std::string_view{name} == "film_print" ? 0.6f : 0.3f);
    EXPECT_EQ(document.Drt()->Params().FilmFormat(), FilmFormat::Film35mm);
  }
}

TEST(PipelineDocumentCheckpointFormat, RoundTripPreservesDefaultIdentityAndIndependentUnlocks) {
  auto document = MultiGradeDocument();
  const auto default_id = document.DefaultGradeId();
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->SetDisplayName("Renamed default");
  grade->SetDeletionProtected(false);
  grade->SetMaskDeletionProtected(MaskId{"mask.radial"}, false);
  grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.locked"});
  grade->SetMaskDeletionProtected(MaskId{"mask.locked"}, true);
  auto* ordinary = dynamic_cast<ColorGradeNodeModel*>(
      document.Graph().FindNode(NodeId{"grade.look"}));
  ASSERT_NE(ordinary, nullptr);
  ordinary->SetDisplayName("Color Grade 1");
  ordinary->SetDeletionProtected(true);
  const auto root_id = ComputeRootId(7, document, std::nullopt);
  const auto encoded = EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), document);
  const auto decoded = DecodePipelineDocumentCheckpoint(encoded);
  EXPECT_EQ(decoded.document.DefaultGradeId(), default_id);
  const auto* restored = dynamic_cast<const ColorGradeNodeModel*>(
      decoded.document.Graph().FindNode(default_id));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->DisplayName(), "Renamed default");
  EXPECT_FALSE(restored->DeletionProtected());
  ASSERT_NE(restored->FindMask(MaskId{"mask.radial"}), nullptr);
  ASSERT_NE(restored->FindMask(MaskId{"mask.locked"}), nullptr);
  EXPECT_FALSE(restored->FindMask(MaskId{"mask.radial"})->deletion_protected);
  EXPECT_TRUE(restored->FindMask(MaskId{"mask.locked"})->deletion_protected);
  const auto* restored_ordinary = dynamic_cast<const ColorGradeNodeModel*>(
      decoded.document.Graph().FindNode(NodeId{"grade.look"}));
  ASSERT_NE(restored_ordinary, nullptr);
  EXPECT_TRUE(restored_ordinary->DeletionProtected());
  EXPECT_EQ(decoded.document.ToJson(), document.ToJson());

  document.SetDefaultGradeId(NodeId{});
  const auto without_default = DecodePipelineDocumentCheckpoint(EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), document));
  EXPECT_TRUE(without_default.document.DefaultGradeId().Empty());
  EXPECT_EQ(without_default.document.ToJson(), document.ToJson());
}

TEST(PipelineDocumentCheckpointFormat, RootAndCheckpointRoundTripRestoreMaskRanges) {
  auto  document = MultiGradeDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const MaskId              mask_id{"mask.radial"};
  const auto                unranged_root_id = ComputeRootId(7, document, std::nullopt);
  const LuminanceRangeModel luminance{0.25f, 0.75f, 0.05f};
  const ColorRangeModel     hue{330.0f, 30.0f, 12.0f};
  grade->SetMaskLuminanceRange(mask_id, luminance);
  grade->SetMaskColorRange(mask_id, hue);
  const auto root_id = ComputeRootId(7, document, std::nullopt);
  EXPECT_NE(root_id, unranged_root_id);

  // Reopen: the root state decodes to the same document and the same root id.
  const auto root = DecodePipelineRootState(EncodePipelineRootState(7, document, std::nullopt));
  EXPECT_EQ(ComputeRootId(7, root.document, root.raw_color_context), root_id);
  const auto* root_grade = root.document.PrimaryGrade();
  ASSERT_NE(root_grade, nullptr);
  ASSERT_NE(root_grade->FindMask(mask_id), nullptr);
  EXPECT_EQ(root_grade->FindMask(mask_id)->luminance_range, luminance);
  EXPECT_EQ(root_grade->FindMask(mask_id)->color_range, hue);

  // Version checkpoint: the stored document restores both ranges byte-identically.
  const auto checkpoint = DecodePipelineDocumentCheckpoint(EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), document));
  EXPECT_EQ(checkpoint.document.ToJson().dump(), document.ToJson().dump());
  ASSERT_NE(checkpoint.document.PrimaryGrade()->FindMask(mask_id), nullptr);
  EXPECT_EQ(checkpoint.document.PrimaryGrade()->FindMask(mask_id)->color_range, hue);

  grade->SetMaskLuminanceRange(mask_id, std::nullopt);
  grade->SetMaskColorRange(mask_id, std::nullopt);
  EXPECT_EQ(ComputeRootId(7, document, std::nullopt), unranged_root_id);
}

TEST(PipelineDocumentCheckpointFormat, RejectsMissingOrMalformedGradeAndMaskProtection) {
  const auto document = MultiGradeDocument();
  const auto root_id = ComputeRootId(7, document, std::nullopt);
  const auto encoded = EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), document);
  const std::vector<nlohmann::json> non_booleans{
      nullptr, 0, 1, "false", nlohmann::json::array(), nlohmann::json::object()};
  for (const bool mask_field : {false, true}) {
    SCOPED_TRACE(mask_field ? "Mask protection" : "Grade protection");
    const auto protection_owner = [&](nlohmann::json& checkpoint) -> nlohmann::json& {
      for (auto& node : checkpoint.at("pipeline_document").at("nodes")) {
        if (node.at("id") == std::string{document.DefaultGradeId().Value()}) {
          return mask_field ? node.at("masks").at(0) : node;
        }
      }
      throw std::runtime_error("Default Grade missing from checkpoint fixture");
    };
    auto missing = encoded;
    protection_owner(missing).erase("deletion_protected");
    EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(missing), std::runtime_error);
    for (const auto& value : non_booleans) {
      SCOPED_TRACE(value.dump());
      auto malformed = encoded;
      protection_owner(malformed)["deletion_protected"] = value;
      EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(malformed), std::runtime_error);
    }
  }
}

TEST(PipelineDocumentCheckpointFormat, RejectsMissingOrInvalidDefaultGradeIdentity) {
  const auto document = MultiGradeDocument();
  const auto root_id = ComputeRootId(7, document, std::nullopt);
  const auto encoded = EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), document);
  auto missing = encoded;
  missing.at("pipeline_document").erase("default_grade_id");
  EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(missing), std::runtime_error);
  const std::vector<nlohmann::json> invalid_ids{
      "", "grade.missing", "develop", "drt", "mask.radial", false, 1,
      nlohmann::json::array(), nlohmann::json::object()};
  for (const auto& id : invalid_ids) {
    SCOPED_TRACE(id.dump());
    auto malformed = encoded;
    malformed.at("pipeline_document")["default_grade_id"] = id;
    EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(malformed), std::exception);
  }
}


TEST(PipelineDocumentCheckpointFormat, OldDocumentFormatIsRejectedWithoutConversion) {
  auto json = CreateDefaultPipelineDocument().ToJson();
  json["format_version"] = 3;
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);
}

TEST(PipelineDocumentCheckpointFormat, OldRootAndCheckpointFormatVersionsAreRejected) {
  const auto document = CreateDefaultPipelineDocument();
  auto       root     = EncodePipelineRootState(9, document, std::nullopt);
  root["root_state_format_version"] = 1;
  EXPECT_THROW((void)DecodePipelineRootState(root), std::runtime_error);

  auto params_root = nlohmann::json{{"state_format_version", 1},
                                    {"pipeline_params", nlohmann::json{{"exposure", 1.5f}}}};
  EXPECT_THROW((void)DecodePipelineRootState(params_root), std::runtime_error);

  const auto root_id = ComputeRootId(9, document, std::nullopt);
  auto checkpoint =
      EncodePipelineDocumentCheckpoint(root_id, std::nullopt, ComputeRootChainHash(root_id),
                                       document);
  checkpoint["checkpoint_state_format_version"] = 1;
  EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(checkpoint), std::runtime_error);

  auto params_checkpoint = nlohmann::json{{"state_format_version", 1},
                                          {"root_id", root_id.ToString()},
                                          {"head_commit_hash", ""},
                                          {"transaction_chain_hash",
                                           ComputeRootChainHash(root_id).ToString()},
                                          {"pipeline_params", nlohmann::json{{"exposure", 1.5f}}}};
  EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(params_checkpoint), std::runtime_error);
}

TEST(PipelineDocumentCheckpointFormat, RootIdChangesWithOwnerOrDocumentContent) {
  const auto document = CreateDefaultPipelineDocument();
  const auto first    = ComputeRootId(11, document, std::nullopt);
  const auto second   = ComputeRootId(12, document, std::nullopt);
  EXPECT_NE(first, second);

  auto other = ClonePipelineDocument(document);
  dynamic_cast<ExposureModel*>(other.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
      ->SetValue(2.25f);
  EXPECT_NE(ComputeRootId(11, other, std::nullopt), first);
}

TEST(PipelineDocumentCheckpointFormat, DifferentImageDevelopDataProducesDifferentRootIdentity) {
  auto left  = CreateDefaultPipelineDocument();
  auto right = ClonePipelineDocument(left);
  auto payload = right.Develop()->Params().Params();
  payload.custom_cct = 4800.0f;
  right.Develop()->Params().ReplaceParams(std::move(payload));

  const auto same_owner = ComputeRootId(11, left, std::nullopt);
  EXPECT_NE(ComputeRootId(11, right, std::nullopt), same_owner);
  EXPECT_EQ(ComputeRootId(11, left, std::nullopt), same_owner);

  const nlohmann::json first_raw{{"CameraModel", "A"}};
  const nlohmann::json second_raw{{"CameraModel", "B"}};
  EXPECT_NE(ComputeRootId(11, left, first_raw), ComputeRootId(11, left, second_raw));
}

TEST(PipelineDocumentCheckpointFormat, ImageRootStoresCompleteDefaultDocumentAndDevelopData) {
  auto document = CreateDefaultPipelineDocument();
  auto payload  = document.Develop()->Params().Params();
  payload.custom_cct = 5100.0f;
  payload.camera_profile.color_matrices_valid = true;
  payload.camera_profile.color_matrix_1[0]    = 0.5;
  document.Develop()->Params().ReplaceParams(std::move(payload));
  const nlohmann::json raw{{"CameraModel", "StoredRoot"}};
  const auto encoded = EncodePipelineRootState(21, document, raw);
  EXPECT_FALSE(encoded.contains("pipeline_params"));
  EXPECT_TRUE(encoded.contains("pipeline_document"));
  EXPECT_EQ(encoded.at("pipeline_document").at("format_version").get<std::uint32_t>(),
            kPipelineDocumentFormatVersion);
  EXPECT_EQ(encoded.at("pipeline_document").at("nodes").size(), 3U);

  auto changed_defaults = CreateDefaultPipelineDocument();
  dynamic_cast<ExposureModel*>(
      changed_defaults.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()))
      ->SetValue(9.0f);
  const auto decoded = DecodePipelineRootState(encoded);
  EXPECT_EQ(decoded.document.ToJson().dump(), document.ToJson().dump());
  EXPECT_NE(decoded.document.ToJson().dump(), changed_defaults.ToJson().dump());
  EXPECT_FLOAT_EQ(decoded.document.Develop()->Params().Params().custom_cct, 5100.0f);
  EXPECT_DOUBLE_EQ(decoded.document.Develop()->Params().Params().camera_profile.color_matrix_1[0],
                   0.5);
}

TEST(PipelineDocumentCheckpointFormat, ExtraRootStateKeysAreRejected) {
  const auto document = CreateDefaultPipelineDocument();
  auto       encoded  = EncodePipelineRootState(3, document, std::nullopt);
  encoded["extra"]    = true;
  EXPECT_THROW((void)DecodePipelineRootState(encoded), std::runtime_error);
}

TEST(MiniGitJournalFormat, OldWalFormatVersionIsRejected) {
  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_chain_hash = ComputeRootChainHash(Hash128{1, 2});
  record.target_chain_hash          = record.expected_source_chain_hash;
  const auto path = std::filesystem::temp_directory_path() / "alcedo-mini-git-old-format.wal";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  {
    nlohmann::json payload{{"format_version", 2},
                           {"sequence", 1},
                           {"kind", 1},
                           {"expected_source_head", ""},
                           {"expected_source_chain_hash",
                            record.expected_source_chain_hash.ToString()},
                           {"target_head", ""},
                           {"target_chain_hash", record.target_chain_hash.ToString()},
                           {"edit_commit", nullptr}};
    const auto checksum = Hash128::Compute(payload.dump().data(), payload.dump().size());
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << nlohmann::json{{"record", payload}, {"checksum", checksum.ToString()}}.dump()
           << '\n';
  }
  MiniGitJournal journal(path);
  std::string    error;
  EXPECT_FALSE(journal.Load(&error));
  EXPECT_NE(error.find("format version"), std::string::npos);
  std::filesystem::remove(path, ignored);
}

TEST(MiniGitJournalFormat, CurrentWalRecordExpectedSerializedRemainsStable) {
  const auto expected_serialized = nlohmann::json::parse(LoadExpectedBytes("mini_git_wal_record.json"));
  MiniGitJournalRecord record;
  record.kind = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_chain_hash =
      Hash128::FromString(expected_serialized.at("expected_source_chain_hash").get<std::string>());
  record.target_chain_hash = record.expected_source_chain_hash;
  const auto path = std::filesystem::temp_directory_path() / "alcedo-mini-git-current-format.wal";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  MiniGitJournal journal(path);
  std::string    error;
  ASSERT_TRUE(journal.Append(record, &error)) << error;
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input);
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
  const auto frame = nlohmann::json::parse(line);
  EXPECT_EQ(frame.at("record").at("format_version").get<std::uint32_t>(),
            kMiniGitJournalRecordFormatVersion);
  EXPECT_EQ(frame.at("record").dump(), LoadExpectedBytes("mini_git_wal_record.json"));
  std::filesystem::remove(path, ignored);
}

TEST(PipelineHistoryFormatTable, OldDocumentCommitRootCheckpointAndWalFormatsFail) {
  auto document_json = CreateDefaultPipelineDocument().ToJson();
  document_json["format_version"] = 3;
  EXPECT_THROW((void)PipelineDocument::FromJson(document_json), std::runtime_error);

  const auto document = CreateDefaultPipelineDocument();
  auto       root     = EncodePipelineRootState(9, document, std::nullopt);
  root["root_state_format_version"] = 1;
  EXPECT_THROW((void)DecodePipelineRootState(root), std::runtime_error);

  auto params_root = nlohmann::json{{"state_format_version", 1},
                                    {"pipeline_params", nlohmann::json{{"exposure", 1.5f}}}};
  EXPECT_THROW((void)DecodePipelineRootState(params_root), std::runtime_error);

  const auto root_id = ComputeRootId(9, document, std::nullopt);
  auto checkpoint =
      EncodePipelineDocumentCheckpoint(root_id, std::nullopt, ComputeRootChainHash(root_id),
                                       document);
  checkpoint["checkpoint_state_format_version"] = 1;
  EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(checkpoint), std::runtime_error);

  auto params_checkpoint =
      nlohmann::json{{"state_format_version", 1},
                     {"root_id", root_id.ToString()},
                     {"head_commit_hash", ""},
                     {"transaction_chain_hash", ComputeRootChainHash(root_id).ToString()},
                     {"pipeline_params", nlohmann::json{{"exposure", 1.5f}}}};
  EXPECT_THROW((void)DecodePipelineDocumentCheckpoint(params_checkpoint), std::runtime_error);

  nlohmann::json image_state{{"element_id", 9},
                             {"root_id", root_id.ToString()},
                             {"active_version_id", Hash128{3, 4}.ToString()},
                             {"materialized_head_commit_hash", ""},
                             {"materialized_transaction_chain_hash",
                              ComputeRootChainHash(root_id).ToString()},
                             {"project_schema_version", 1}};
  EXPECT_THROW((void)ImageEditState::FromJSON(image_state), std::runtime_error);

  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.3.0"));

  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_chain_hash = ComputeRootChainHash(root_id);
  record.target_chain_hash          = record.expected_source_chain_hash;
  const auto path = std::filesystem::temp_directory_path() / "alcedo-mini-git-combined-old.wal";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  {
    nlohmann::json payload{{"format_version", 2},
                           {"sequence", 1},
                           {"kind", 1},
                           {"expected_source_head", ""},
                           {"expected_source_chain_hash",
                            record.expected_source_chain_hash.ToString()},
                           {"target_head", ""},
                           {"target_chain_hash", record.target_chain_hash.ToString()},
                           {"edit_commit", nullptr}};
    const auto checksum = Hash128::Compute(payload.dump().data(), payload.dump().size());
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << nlohmann::json{{"record", payload}, {"checksum", checksum.ToString()}}.dump()
           << '\n';
  }
  MiniGitJournal journal(path);
  std::string    error;
  EXPECT_FALSE(journal.Load(&error));
  EXPECT_NE(error.find("format version"), std::string::npos);
  std::filesystem::remove(path, ignored);
}

}  // namespace alcedo
