//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_document_checkpoint.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <vector>
#include "app/project_package_backend.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
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

auto MultiGradeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  EXPECT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
  grade_mask_test::AddRadialMask(document, MaskId{"mask.radial"});
  return document;
}

}  // namespace

TEST(PipelineHistoryFormatTable, PublishedConstantsIdentifyTheDocumentHistoryCutover) {
  EXPECT_EQ(kProjectFileVersion, "0.10.0");
  EXPECT_EQ(kMinSupportedProjectFileVersion, kProjectFileVersion);
  EXPECT_EQ(kMaxSupportedProjectFileVersion, kProjectFileVersion);
  EXPECT_EQ(kPackedProjectFormatVersion, 7u);
  EXPECT_EQ(kPipelineDocumentFormatVersion, 7u);
  EXPECT_EQ(kImageEditSchemaVersion, 5u);
  EXPECT_EQ(kCommitFormatVersion, 5u);
  EXPECT_EQ(kChainFormatVersion, 5u);
  EXPECT_EQ(kPipelineEditBatchFormatVersion, 4u);
  EXPECT_EQ(kRootStateFormatVersion, 5u);
  EXPECT_EQ(kCheckpointStateFormatVersion, 5u);
  EXPECT_EQ(kMiniGitJournalRecordFormatVersion, 6u);
  EXPECT_EQ(kAdjustmentTransferSchema, "alcedo.adjustment_transfer.v6");
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

TEST(PipelineDocumentCheckpointFormat, RoundTripPreservesDefaultIdentityAndIndependentUnlocks) {
  auto document = MultiGradeDocument();
  const auto default_id = document.DefaultGradeId();
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->SetDisplayName("Renamed default");
  grade->SetDeletionProtected(false);
  grade->SetMaskDeletionProtected(MaskId{"mask.radial"}, false);
  auto& protected_mask = grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.locked"});
  protected_mask.deletion_protected = true;
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
