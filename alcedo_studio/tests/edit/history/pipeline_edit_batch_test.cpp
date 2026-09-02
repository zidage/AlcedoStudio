//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <clocale>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/history/commit_clock_test_access.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/op_base.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

constexpr char kOldRootChainHash[]   = "b086b9015c867f88aeca8730b1b8d55c";
constexpr char kOldOrdinaryCommitHash[] = "02c397162017dc758e0c06ed5b9e0529";

auto LoadGolden(const std::string& name) -> std::string {
  const std::filesystem::path path =
      std::filesystem::path(PIPELINE_EDIT_BATCH_GOLDEN_DIR) / name;
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input) << path.string();
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void AppendU64LE(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void AppendHash(std::vector<std::uint8_t>& out, const Hash128& hash) {
  const auto bytes = hash.ToBytes();
  out.insert(out.end(), bytes.begin(), bytes.end());
}

auto IndependentRootChainInput(const root_id_t& root_id) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  AppendU32LE(bytes, 2u);
  AppendHash(bytes, root_id);
  return bytes;
}

auto IndependentCommitHashInput(const root_id_t& root_id, std::uint64_t created_at_ns,
                                const std::string& payload_dump) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  AppendU32LE(bytes, 2u);
  AppendHash(bytes, root_id);
  bytes.push_back(0);  // first parent absent
  bytes.push_back(0);  // second parent absent
  AppendU64LE(bytes, created_at_ns);
  bytes.push_back(0);  // fixed kind = edit
  bytes.insert(bytes.end(), payload_dump.begin(), payload_dump.end());
  return bytes;
}

auto MakeParameterTarget() -> PipelineParameterTarget {
  PipelineParameterTarget target;
  target.owner_kind              = PipelineParameterOwnerKind::ColorGrade;
  target.node_id                 = NodeId{"grade.primary"};
  target.adjustment_instance_id  = AdjustmentInstanceId{"grade.primary.exposure"};
  target.field_key               = "exposure";
  return target;
}

auto MakeParameterChange(double before, double after) -> SetParameterChange {
  SetParameterChange change;
  change.target         = MakeParameterTarget();
  change.before_value   = nlohmann::json{{"exposure_ev", before}};
  change.after_value    = nlohmann::json{{"exposure_ev", after}};
  change.before_enabled = true;
  change.after_enabled  = true;
  return change;
}

auto MakeParameterBatch(double before, double after) -> PipelineEditBatch {
  nlohmann::json args{{"field_key", "exposure"},
                      {"node_display_name", "Color Grade"},
                      {"node_id", "grade.primary"}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                 {MakeParameterChange(before, after)},
                                 "history.operation.set_parameter", std::move(args));
}

auto MakeRadialMask(MaskId id, std::string display_name) -> MaskModel {
  MaskModel mask;
  mask.id           = std::move(id);
  mask.display_name = std::move(display_name);
  mask.source       = RadialMaskSource{};
  return mask;
}

auto MakeBrushMask(MaskId id, std::string display_name, std::string asset_key) -> MaskModel {
  MaskModel mask;
  mask.id           = std::move(id);
  mask.display_name = std::move(display_name);
  BrushMaskSource brush;
  brush.asset_key              = MaskAssetKey{std::move(asset_key)};
  brush.descriptor.extent      = {1, 1};
  brush.descriptor.reference_bounds = {};
  mask.source                  = std::move(brush);
  return mask;
}

auto MakeLookNodeJson() -> nlohmann::json {
  auto node = std::make_unique<ColorGradeNodeModel>(NodeId{"grade.look"});
  node->SetDisplayName("Look");
  auto exposure = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  dynamic_cast<ExposureModel*>(exposure.get())->SetValue(0.5f);
  node->InsertAdjustment(0, AdjustmentInstanceId{"grade.look.exposure"}, std::move(exposure));
  node->AddMask(MakeRadialMask(MaskId{"mask.radial"}, "Radial"), 0);
  node->AddMask(
      MakeBrushMask(MaskId{"mask.brush"}, "Brush", "0123456789abcdef0123456789abcdef"), 1);
  return node->ToJson();
}

auto MakeSceneEdge(std::string_view from, std::string_view to) -> PipelineSceneEdge {
  return PipelineSceneEdge{NodeId{std::string{from}}, PortId{"image"}, NodeId{std::string{to}},
                           PortId{"image"}};
}

auto MakeRemoveGradeBatch() -> PipelineEditBatch {
  RemoveColorGradeChange change;
  change.node_id               = NodeId{"grade.look"};
  change.node                  = MakeLookNodeJson();
  change.predecessor_id        = NodeId{"develop"};
  change.successor_id          = NodeId{"drt"};
  change.removed_incoming_edge = MakeSceneEdge("develop", "grade.look");
  change.removed_outgoing_edge = MakeSceneEdge("grade.look", "drt");
  change.bridge_edge           = MakeSceneEdge("develop", "drt");
  nlohmann::json args{{"node_display_name", "Look"}, {"node_id", "grade.look"}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::RemoveColorGrade, {std::move(change)},
                                 "history.operation.remove_color_grade", std::move(args));
}

auto MakeReconnectBatch() -> PipelineEditBatch {
  ReconnectColorGradeChange change;
  change.node_id                 = NodeId{"grade.b"};
  change.before_predecessor_id   = NodeId{"grade.primary"};
  change.before_successor_id     = NodeId{"drt"};
  change.after_predecessor_id    = NodeId{"develop"};
  change.after_successor_id      = NodeId{"grade.primary"};
  change.before_incoming_edge    = MakeSceneEdge("grade.primary", "grade.b");
  change.before_outgoing_edge    = MakeSceneEdge("grade.b", "drt");
  change.after_incoming_edge     = MakeSceneEdge("develop", "grade.b");
  change.after_outgoing_edge     = MakeSceneEdge("grade.b", "grade.primary");
  nlohmann::json args{{"node_id", "grade.b"}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::ReconnectColorGrade, {std::move(change)},
                                 "history.operation.reconnect_color_grade", std::move(args));
}

auto MakeBrushReplaceBatch() -> PipelineEditBatch {
  ReplaceMaskAssetChange change;
  change.node_id = NodeId{"grade.look"};
  change.mask_id = MaskId{"mask.brush"};
  change.before_source =
      MaskModelToJson(MakeBrushMask(MaskId{"mask.brush"}, "Brush",
                                   "0123456789abcdef0123456789abcdef"))
          .at("source");
  change.after_source =
      MaskModelToJson(MakeBrushMask(MaskId{"mask.brush"}, "Brush",
                                   "fedcba9876543210fedcba9876543210"))
          .at("source");
  nlohmann::json args{{"mask_display_name", "Brush"},
                      {"mask_id", "mask.brush"},
                      {"node_id", "grade.look"}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::ReplaceMaskAsset, {std::move(change)},
                                 "history.operation.replace_mask_asset", std::move(args));
}

void ExpectRoundTrip(const PipelineEditBatch& batch) {
  const auto json     = batch.CanonicalJSON();
  const auto restored = PipelineEditBatch::FromJSON(json);
  EXPECT_EQ(restored.CanonicalJSON().dump(), json.dump());
}

}  // namespace

TEST(PipelineEditBatch, TypedBatchGoldenBytesAndHashRemainStable) {
  const auto golden = LoadGolden("set_parameter_batch.json");
  const auto parsed = nlohmann::json::parse(golden);
  EXPECT_FALSE(parsed.contains("kind"));
  EXPECT_FALSE(parsed.contains("operator_type"));
  EXPECT_FALSE(parsed.contains("stage_name"));
  EXPECT_FALSE(parsed.at("changes").at(0).contains("operator_type"));
  EXPECT_FALSE(parsed.at("changes").at(0).contains("stage_name"));
  const auto batch  = PipelineEditBatch::FromJSON(parsed);
  EXPECT_EQ(batch.CanonicalJSON().dump(), golden);

  const root_id_t root{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
  const auto      commit = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      root, std::nullopt, 42, batch);
  const auto golden_input = IndependentCommitHashInput(root, 42, golden);
  EXPECT_EQ(commit.CanonicalHashInput(), golden_input);
  const auto expected_hash = Hash128::Compute(golden_input.data(), golden_input.size());
  EXPECT_EQ(commit.GetCommitHash(), expected_hash);
  EXPECT_EQ(expected_hash.ToString(), LoadGolden("set_parameter_commit_hash.txt"));

  const auto root_input = IndependentRootChainInput(root);
  const auto root_chain = Hash128::Compute(root_input.data(), root_input.size());
  EXPECT_EQ(ComputeRootChainHash(root), root_chain);
  EXPECT_EQ(root_chain.ToString(), LoadGolden("root_chain_hash.txt"));
  EXPECT_NE(root_chain.ToString(), kOldRootChainHash);

  const auto fold_input = [&]() {
    std::vector<std::uint8_t> bytes;
    AppendHash(bytes, root_chain);
    AppendHash(bytes, commit.GetCommitHash());
    return bytes;
  }();
  const auto folded = Hash128::Compute(fold_input.data(), fold_input.size());
  EXPECT_EQ(FoldTransactionChainHash(root_chain, commit.GetCommitHash()), folded);
  EXPECT_EQ(folded.ToString(), LoadGolden("set_parameter_chain_hash.txt"));
}

TEST(PipelineEditBatch, TypedCommitAndChainGoldenIdentitySurvivesLegacyRemoval) {
  const auto golden = LoadGolden("set_parameter_batch.json");
  const auto parsed = nlohmann::json::parse(golden);
  EXPECT_FALSE(parsed.contains("kind"));
  EXPECT_FALSE(parsed.contains("operator_type"));
  EXPECT_FALSE(parsed.contains("stage_name"));
  EXPECT_FALSE(parsed.contains("merge_field_keys"));
  EXPECT_FALSE(parsed.contains("conflicts"));
  const auto batch  = PipelineEditBatch::FromJSON(parsed);
  EXPECT_EQ(batch.CanonicalJSON().dump(), golden);

  const root_id_t root{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
  const auto      commit = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      root, std::nullopt, 42, batch);
  const auto commit_json = commit.ToJSON();
  EXPECT_EQ(commit_json.at("kind").get<std::string>(), "edit");
  EXPECT_EQ(commit_json.at("second_parent_hash").get<std::string>(), "");
  EXPECT_FALSE(commit_json.contains("merge_field_keys"));
  EXPECT_FALSE(commit_json.contains("operator_type"));
  EXPECT_FALSE(commit_json.contains("stage_name"));
  EXPECT_EQ(commit_json.at("edit_payload").dump(), golden);

  const auto golden_input = IndependentCommitHashInput(root, 42, golden);
  EXPECT_EQ(commit.CanonicalHashInput(), golden_input);
  const auto expected_hash = Hash128::Compute(golden_input.data(), golden_input.size());
  EXPECT_EQ(commit.GetCommitHash(), expected_hash);
  EXPECT_EQ(expected_hash.ToString(), LoadGolden("set_parameter_commit_hash.txt"));
  EXPECT_NE(expected_hash.ToString(), kOldOrdinaryCommitHash);

  const auto root_input = IndependentRootChainInput(root);
  const auto root_chain = Hash128::Compute(root_input.data(), root_input.size());
  EXPECT_EQ(ComputeRootChainHash(root), root_chain);
  EXPECT_EQ(root_chain.ToString(), LoadGolden("root_chain_hash.txt"));

  const auto folded = FoldTransactionChainHash(root_chain, commit.GetCommitHash());
  EXPECT_EQ(folded.ToString(), LoadGolden("set_parameter_chain_hash.txt"));
}

TEST(PipelineEditBatch, RemoveColorGradeGoldenBytesRemainStable) {
  const auto golden = LoadGolden("remove_color_grade_batch.json");
  const auto batch  = MakeRemoveGradeBatch();
  EXPECT_EQ(batch.CanonicalJSON().dump(), golden);
  EXPECT_EQ(PipelineEditBatch::FromJSON(nlohmann::json::parse(golden)).CanonicalJSON().dump(),
            golden);
}

TEST(PipelineEditBatch, ReconnectGoldenBytesRemainStable) {
  const auto golden = LoadGolden("reconnect_color_grade_batch.json");
  const auto batch  = MakeReconnectBatch();
  EXPECT_EQ(batch.CanonicalJSON().dump(), golden);
  EXPECT_EQ(PipelineEditBatch::FromJSON(nlohmann::json::parse(golden)).CanonicalJSON().dump(),
            golden);
}

TEST(PipelineEditBatch, BrushAssetReplacementGoldenBytesRemainStable) {
  const auto golden = LoadGolden("replace_mask_asset_batch.json");
  const auto batch  = MakeBrushReplaceBatch();
  EXPECT_EQ(batch.CanonicalJSON().dump(), golden);
  EXPECT_EQ(PipelineEditBatch::FromJSON(nlohmann::json::parse(golden)).CanonicalJSON().dump(),
            golden);
}

TEST(PipelineEditBatch, RoundTripForEveryChangeVariant) {
  ExpectRoundTrip(MakeParameterBatch(0.0, 1.25));

  SetNodeEnabledChange enabled;
  enabled.node_id        = NodeId{"grade.primary"};
  enabled.node_kind      = PipelineEditNodeKind::ColorGrade;
  enabled.before_enabled = true;
  enabled.after_enabled  = false;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeEnabled, {enabled},
                                          "history.operation.set_node_enabled"));

  SetNodeMixChange mix;
  mix.node_id    = NodeId{"grade.primary"};
  mix.before_mix = 1.0f;
  mix.after_mix  = 0.5f;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeMix, {mix},
                                          "history.operation.set_node_mix"));

  RenameColorGradeChange rename;
  rename.node_id             = NodeId{"grade.primary"};
  rename.before_display_name = "Color Grade";
  rename.after_display_name  = "Look";
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::RenameColorGrade, {rename},
                                          "history.operation.rename_color_grade"));

  AddColorGradeChange add;
  add.node_id        = NodeId{"grade.look"};
  add.node           = MakeLookNodeJson();
  add.predecessor_id = NodeId{"develop"};
  add.successor_id   = NodeId{"drt"};
  add.incoming_edge  = MakeSceneEdge("develop", "grade.look");
  add.outgoing_edge  = MakeSceneEdge("grade.look", "drt");
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::AddColorGrade, {add},
                                          "history.operation.add_color_grade"));

  ExpectRoundTrip(MakeRemoveGradeBatch());
  ExpectRoundTrip(MakeReconnectBatch());

  AddMaskChange add_mask;
  add_mask.node_id       = NodeId{"grade.look"};
  add_mask.mask_id       = MaskId{"mask.radial"};
  add_mask.mask          = MaskModelToJson(MakeRadialMask(MaskId{"mask.radial"}, "Radial"));
  add_mask.display_index = 0;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::AddMask, {add_mask},
                                          "history.operation.add_mask"));

  RemoveMaskChange remove_mask = {};
  remove_mask.node_id          = NodeId{"grade.look"};
  remove_mask.mask_id          = MaskId{"mask.radial"};
  remove_mask.mask = MaskModelToJson(MakeRadialMask(MaskId{"mask.radial"}, "Radial"));
  remove_mask.display_index = 0;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::RemoveMask, {remove_mask},
                                          "history.operation.remove_mask"));

  ReplaceMaskSourceChange source;
  source.node_id = NodeId{"grade.look"};
  source.mask_id = MaskId{"mask.radial"};
  source.before_source =
      MaskModelToJson(MakeRadialMask(MaskId{"mask.radial"}, "Radial")).at("source");
  LinearGradientMaskSource gradient;
  MaskModel                linear;
  linear.id           = MaskId{"mask.radial"};
  linear.display_name = "Linear";
  linear.source       = gradient;
  source.after_source = MaskModelToJson(linear).at("source");
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::ReplaceMaskSource, {source},
                                          "history.operation.replace_mask_source"));

  ExpectRoundTrip(MakeBrushReplaceBatch());

  SetMaskFieldChange field;
  field.node_id      = NodeId{"grade.look"};
  field.mask_id      = MaskId{"mask.radial"};
  field.field_key    = "opacity";
  field.before_value = 1.0;
  field.after_value  = 0.25;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::SetMaskField, {field},
                                          "history.operation.set_mask_field"));

  SetParameterChange document;
  document.target.owner_kind = PipelineParameterOwnerKind::Document;
  document.target.field_key  = "working_color_space";
  document.before_value      = nlohmann::json{{"working_color_space", "acescg"}};
  document.after_value       = nlohmann::json{{"working_color_space", "rec2020"}};
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {document},
                                          "history.operation.set_parameter"));

  SetParameterChange mask_parameter;
  mask_parameter.target.owner_kind = PipelineParameterOwnerKind::ColorGradeMask;
  mask_parameter.target.node_id    = NodeId{"grade.look"};
  mask_parameter.target.mask_id    = MaskId{"mask.radial"};
  mask_parameter.target.field_key  = "opacity";
  mask_parameter.before_value      = nlohmann::json{{"opacity", 1.0}};
  mask_parameter.after_value       = nlohmann::json{{"opacity", 0.4}};
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {mask_parameter},
                                          "history.operation.set_parameter"));

  AddMaskChange paste_mask;
  paste_mask.node_id       = NodeId{"grade.look"};
  paste_mask.mask_id       = MaskId{"mask.radial"};
  paste_mask.mask          = MaskModelToJson(MakeRadialMask(MaskId{"mask.radial"}, "Radial"));
  paste_mask.display_index = 0;
  ExpectRoundTrip(PipelineEditBatch::Make(PipelineEditOperationKind::Paste,
                                          {add, paste_mask}, "history.operation.paste"));
}

TEST(PipelineEditBatch, ChangingTypedChangeOrderChangesCommitIdentity) {
  auto first  = MakeParameterChange(0.0, 1.25);
  auto second = MakeParameterChange(1.25, 0.5);
  second.target.field_key              = "contrast";
  second.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.contrast"};
  second.before_value                  = nlohmann::json{{"contrast", 0.0}};
  second.after_value                   = nlohmann::json{{"contrast", 20.0}};

  const auto forward = PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                               {first, second}, "history.operation.set_parameter");
  const auto reversed = PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                                {second, first}, "history.operation.set_parameter");
  EXPECT_NE(forward.CanonicalJSON().dump(), reversed.CanonicalJSON().dump());

  const root_id_t root{1, 2};
  const auto      a = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      root, std::nullopt, 7, forward);
  const auto b = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      root, std::nullopt, 7, reversed);
  EXPECT_NE(a.GetCommitHash(), b.GetCommitHash());
}

TEST(PipelineEditBatch, UnknownOrMissingTypedPayloadFieldsAreRejected) {
  auto json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["extra"] = true;
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json.erase("presentation_key");
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["operation_kind"] = "unknown_kind";
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["changes"][0]["kind"] = "not_a_change";
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["changes"][0]["extra"] = true;
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeRemoveGradeBatch().CanonicalJSON();
  json["changes"][0]["node"]["extra"] = true;
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["changes"][0]["target"]["owner_kind"] = "stage";
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["changes"] = nlohmann::json::array();
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);

  json = MakeParameterBatch(0.0, 1.25).CanonicalJSON();
  json["batch_format_version"] = 99;
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(json), std::runtime_error);
}

TEST(PipelineEditBatch, ParameterHistoryRequiresCompleteOwnerNodeAndInstance) {
  SetParameterChange incomplete;
  incomplete.before_value = nlohmann::json::object();
  incomplete.after_value  = nlohmann::json{{"exposure_ev", 1.0}};
  incomplete.target.field_key  = "exposure";
  incomplete.target.owner_kind = PipelineParameterOwnerKind::ColorGrade;
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {incomplete},
                                             "history.operation.set_parameter"),
               std::runtime_error);

  incomplete.target.node_id = NodeId{"grade.primary"};
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {incomplete},
                                             "history.operation.set_parameter"),
               std::runtime_error);

  incomplete.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  EXPECT_NO_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                                {incomplete}, "history.operation.set_parameter"));

  SetParameterChange mask_target;
  mask_target.target.owner_kind = PipelineParameterOwnerKind::ColorGradeMask;
  mask_target.target.node_id    = NodeId{"grade.look"};
  mask_target.target.field_key  = "opacity";
  mask_target.before_value      = nlohmann::json{{"opacity", 1.0}};
  mask_target.after_value       = nlohmann::json{{"opacity", 0.5}};
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {mask_target},
                                             "history.operation.set_parameter"),
               std::runtime_error);
  mask_target.target.mask_id = MaskId{"mask.radial"};
  EXPECT_NO_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                                {mask_target}, "history.operation.set_parameter"));
}

TEST(PipelineEditBatch, OrdinaryAndMergePayloadsAreRejected) {
  nlohmann::json ordinary = {
      {"operator_type", 1},
      {"stage_name", 2},
      {"field_name", "exposure"},
      {"before_value", 0.0},
      {"after_value", 1.0},
  };
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(ordinary), std::runtime_error);

  nlohmann::json merge = {
      {"fields", nlohmann::json::array()},
  };
  EXPECT_THROW((void)PipelineEditBatch::FromJSON(merge), std::runtime_error);
}

TEST(PipelineEditBatch, IncompatibleChangeKindIsRejected) {
  SetNodeEnabledChange enabled;
  enabled.node_id   = NodeId{"grade.primary"};
  enabled.node_kind = PipelineEditNodeKind::ColorGrade;
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {enabled},
                                             "history.operation.set_parameter"),
               std::runtime_error);
}

TEST(PipelineEditBatch, NonFiniteMixAndNumbersAreRejected) {
  SetNodeMixChange mix;
  mix.node_id    = NodeId{"grade.primary"};
  mix.before_mix = 1.0f;
  mix.after_mix  = std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeMix, {mix},
                                             "history.operation.set_node_mix"),
               std::runtime_error);

  mix.after_mix = 1.5f;
  EXPECT_THROW((void)PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeMix, {mix},
                                             "history.operation.set_node_mix"),
               std::runtime_error);
}

TEST(PipelineEditBatch, OrderedChangesForApplyReverseWithoutDocumentMutation) {
  auto first  = MakeParameterChange(0.0, 1.0);
  auto second = MakeParameterChange(1.0, 2.0);
  second.target.field_key              = "contrast";
  second.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.contrast"};
  second.before_value                  = nlohmann::json{{"contrast", 0.0}};
  second.after_value                   = nlohmann::json{{"contrast", 10.0}};
  const auto batch = PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter,
                                             {first, second}, "history.operation.set_parameter");
  const auto forward =
      OrderedChangesForApply(batch, PipelineEditApplyDirection::Forward);
  const auto inverse =
      OrderedChangesForApply(batch, PipelineEditApplyDirection::Inverse);
  ASSERT_EQ(forward.size(), 2u);
  ASSERT_EQ(inverse.size(), 2u);
  EXPECT_EQ(std::get<SetParameterChange>(forward[0]).target.field_key, "exposure");
  EXPECT_EQ(std::get<SetParameterChange>(forward[1]).target.field_key, "contrast");
  EXPECT_EQ(std::get<SetParameterChange>(inverse[0]).target.field_key, "contrast");
  EXPECT_EQ(std::get<SetParameterChange>(inverse[1]).target.field_key, "exposure");
}

TEST(PipelineEditBatch, TypedHistoryRowsUseSavedIdentityAndLocalizationData) {
  const auto batch = MakeRemoveGradeBatch();
  const auto row   = ProjectPipelineEditHistory(batch);
  EXPECT_EQ(row.operation_kind, PipelineEditOperationKind::RemoveColorGrade);
  EXPECT_EQ(row.presentation_key, "history.operation.remove_color_grade");
  EXPECT_EQ(row.node_id, "grade.look");
  EXPECT_EQ(row.node_display_name, "Look");
  EXPECT_TRUE(row.field_key.empty());
  EXPECT_TRUE(row.adjustment_instance_id.empty());
  EXPECT_FALSE(row.presentation_args.contains("stage_name"));
  EXPECT_FALSE(row.presentation_args.contains("operator_type"));
  EXPECT_EQ(row.presentation_args.at("node_display_name").get<std::string>(), "Look");

  const auto parameter = ProjectPipelineEditHistory(MakeParameterBatch(0.0, 1.25));
  EXPECT_EQ(parameter.field_key, "exposure");
  EXPECT_EQ(parameter.node_id, "grade.primary");
  EXPECT_EQ(parameter.adjustment_instance_id, "grade.primary.exposure");
  EXPECT_EQ(parameter.node_display_name, "Color Grade");
  EXPECT_FALSE(parameter.presentation_args.contains("stage_name"));
  EXPECT_FALSE(parameter.presentation_args.contains("operator_type"));
}

TEST(PipelineEditBatch, LocaleIndependentHashEquality) {
  const auto batch = MakeParameterBatch(0.0, 1.25);
  const auto dump  = batch.CanonicalJSON().dump();
  const char* previous = std::setlocale(LC_ALL, nullptr);
  const char* german   = std::setlocale(LC_ALL, "de-DE");
  if (german == nullptr) {
    german = std::setlocale(LC_ALL, "de_DE.UTF-8");
  }
  const auto localized = batch.CanonicalJSON().dump();
  if (previous != nullptr) {
    std::setlocale(LC_ALL, previous);
  }
  EXPECT_EQ(dump, localized);
  const root_id_t root{3, 4};
  const auto      commit = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      root, std::nullopt, 9, batch);
  EXPECT_EQ(commit.CanonicalHashInput(), IndependentCommitHashInput(root, 9, dump));
}

TEST(PipelineEditBatch, FuzzParseRejectsNonCanonicalPayloads) {
  const auto golden = LoadGolden("set_parameter_batch.json");
  std::mt19937 rng(20260901);
  for (int i = 0; i < 64; ++i) {
    std::string mutated = golden;
    if (mutated.empty()) {
      break;
    }
    const auto index = static_cast<std::size_t>(rng() % mutated.size());
    mutated[index] = static_cast<char>(static_cast<unsigned char>(mutated[index]) ^ 0x20u);
    try {
      const auto parsed = nlohmann::json::parse(mutated);
      const auto batch  = PipelineEditBatch::FromJSON(parsed);
      EXPECT_EQ(batch.CanonicalJSON().dump(), parsed.dump())
          << "non-canonical payload was accepted: " << mutated;
    } catch (const std::exception&) {
    }
  }
}

TEST(PipelineEditBatch, GraphInsertAndTypedCommitVerification) {
  const root_id_t root{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
  EXPECT_NE(ComputeRootChainHash(root).ToString(), kOldRootChainHash);

  auto graph = CommitGraph::CreateEmpty(44);
  const auto typed = edit_history_test::EditCommitAccess::MakePipelineEditAtTimestamp(
      graph.GetRootId(), std::nullopt, 100, MakeParameterBatch(0.0, 1.25));
  ASSERT_TRUE(graph.InsertCommit(typed));
  const auto* stored = graph.FindCommit(typed.GetCommitHash());
  ASSERT_NE(stored, nullptr);
  const auto restored = PipelineEditBatch::FromJSON(stored->GetPayloadJSON());
  EXPECT_EQ(restored.CanonicalJSON().dump(), typed.GetPayloadJSON().dump());
}

}  // namespace alcedo
