//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The RGB curve of Color Grade 1 follows the ProfileToneCurve of the camera profile (#911,
// dcp_profile_tone_curve_study.md, section 11). A settled edit of `camera_profile_file` or of the
// DRT method that starts the profile curve (an entry) writes the fitted points into the RGB curve
// and saturation 1.3 -> 1.0 of Color Grade 1 in the same history batch; a document imported
// before the Film-like curve (#891) also has contrast 15 -> 0. A change of profile or of method
// moves an RGB curve that still has the points before to the new points; an exit puts the RAW
// import look back on such a curve. A curve that the user edited stays.
//
// Each case writes synthetic DCP files into a scratch folder and scans it into
// DcpProfileLibrary::Shared(), the library that the selection write and the reset read.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/editor_history_types.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "edit/runtime/profile_tone_curve_import.hpp"
#include "image/dcp_profile_library.hpp"
#include "image/dcp_test_file_writer.hpp"
#include "json.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

namespace alcedo::ui {
namespace {

using test::ColorGradeFieldTarget;
using test::PatchFromJson;
using test::WithColorGradeTarget;
using test::WithDrtPostTarget;

constexpr std::uint16_t kProfileToneCurveTag = 50940;
constexpr const char*   kCameraModel         = "Alcedo Tone Camera";
constexpr const char*   kCurveProfile        = "Camera Standard";
constexpr const char*   kOtherCurveProfile   = "Camera Vivid";
constexpr const char*   kFlatProfile         = "Camera Flat";

/// Real document, history, and WAL of image 42, and a DCP folder with two profiles that have a
/// `ProfileToneCurve` and one that lacks it.
class EditorProfileToneEntryHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto scratch = std::filesystem::path(TEST_IMG_PATH)
                             .parent_path()
                             .parent_path()
                             .parent_path()
                             .parent_path() /
                         "build/tmp/profile_tone_entry" / stamp;
    journal_path_ = scratch / "document_history.wal";
    dcp_dir_      = scratch / "Camera";
    std::filesystem::create_directories(dcp_dir_);
    WriteProfile(kCurveProfile, {0.0f, 0.0f, 0.18f, 0.44f, 1.0f, 1.0f}, 1.0, "standard.dcp");
    WriteProfile(kOtherCurveProfile, {0.0f, 0.0f, 0.18f, 0.5f, 1.0f, 1.0f}, 1.05, "vivid.dcp");
    WriteProfile(kFlatProfile, {}, 0.95, "flat.dcp");
    ASSERT_EQ(DcpProfileLibrary::Shared().Scan(dcp_dir_).profile_count, 3u);

    lease_    = test::MakeInMemoryEditorLease(42);
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t) { return test::CopyEditorLease(lease_); }});
    history_.SetPipelinePort(pipeline_);
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
  }

  void TearDown() override {
    history_.Release({42, true});
    (void)DcpProfileLibrary::Shared().Scan({});
    std::error_code ec;
    std::filesystem::remove_all(dcp_dir_.parent_path(), ec);
  }

  /// A profile with the ProfileToneCurve pairs @p curve, or without the tag when it is empty.
  void WriteProfile(const char* name, std::vector<float> curve, double matrix_scale,
                    const char* file) {
    auto writer = test::MinimalDcpProfile(kCameraModel, name, matrix_scale);
    if (!curve.empty()) {
      writer.Floats(kProfileToneCurveTag, std::move(curve));
    }
    writer.Write(dcp_dir_ / file);
  }

  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, 42);
  }
  auto Head() -> head_commit_hash_t { return test::EditorWorkingHead(history_, 42); }

  /// Value of the Color Grade 1 field @p field (`contrast` or `saturation`).
  auto GradeValue(const char* field) -> float {
    nlohmann::json json;
    std::string    error;
    EXPECT_TRUE(
        ReadEditorParameterJson(Working()->Document(), ColorGradeFieldTarget(field), &json, &error))
        << error;
    return json.at(field).get<float>();
  }
  auto Reference() -> std::optional<DevelopCameraProfileFile> {
    return Working()->Document().Develop()->Params().CameraProfileFile();
  }
  /// The RGB curve of Color Grade 1.
  auto RgbCurve() -> std::vector<CurvePoint> {
    const auto* curve = dynamic_cast<const CurveModel*>(
        Working()->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Curve()));
    EXPECT_NE(curve, nullptr);
    return curve == nullptr ? std::vector<CurvePoint>{} : curve->Points();
  }
  static auto ClDigitalPoints() -> std::vector<CurvePoint> {
    return FindToneCurvePreset(kDefaultPipelineToneCurvePresetId)->PointList();
  }
  /// The fitted RGB curve of the ProfileToneCurve of @p profile on the DRT @p method.
  static auto ProfileCurve(const char* profile, DrtMethod method = DrtMethod::OpenDrt)
      -> std::vector<CurvePoint> {
    const auto dcp = DcpProfileLibrary::Shared().Find(kCameraModel, profile);
    EXPECT_NE(dcp, nullptr);
    return dcp == nullptr ? std::vector<CurvePoint>{}
                          : ProfileToneCurvePoints(dcp->profile_tone_curve, method)->points;
  }

  /// One settled edit of @p field with @p params, as the editor submits it.
  auto Commit(const EditorHistoryGuardHandle& handle, EditorAdjustmentPatch patch) -> bool {
    std::string error;
    const bool  ok = history_.CaptureAdjustmentBeforePreview(handle, patch, &error) &&
                    history_.CommitAdjustment(handle, patch, &error);
    EXPECT_TRUE(ok) << error;
    return ok;
  }
  auto CommitProfile(const EditorHistoryGuardHandle& handle, const char* profile,
                     bool profile_tone = true) -> bool {
    const nlohmann::json params{
        {"camera_model", kCameraModel}, {"profile_name", profile}, {"profile_tone", profile_tone}};
    return Commit(handle, PatchFromJson("camera_profile_file", params.dump(), true));
  }
  auto CommitGrade(const EditorHistoryGuardHandle& handle, const char* field, float value) -> bool {
    return Commit(handle,
                  WithColorGradeTarget({field, nlohmann::json{{field, value}}.dump(), true}));
  }
  /// One settled write of the RGB curve of Color Grade 1 by preset id.
  auto CommitCurvePreset(const EditorHistoryGuardHandle& handle, const char* preset) -> bool {
    return Commit(handle,
                  WithColorGradeTarget({"curve", nlohmann::json{{"points", preset}}.dump(), true}));
  }
  auto CommitMethod(const EditorHistoryGuardHandle& handle, const char* method) -> bool {
    return Commit(handle,
                  WithDrtPostTarget({"odt", nlohmann::json{{"method", method}}.dump(), true}));
  }

  std::filesystem::path                      journal_path_;
  std::filesystem::path                      dcp_dir_;
  /// History and documents that the next Acquire of image 42 takes (as a copy).
  EditorHistoryLease                         lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorProfileToneEntryHistoryTest, DcpSelectionLoadsTheProfileCurveInOneUndoStep) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_EQ(Working()->Document().Drt()->Params().Method(), DrtMethod::OpenDrt);
  ASSERT_EQ(RgbCurve(), ClDigitalPoints());
  ASSERT_FLOAT_EQ(GradeValue("contrast"), 0.0f);
  ASSERT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
  const auto profile_curve = ProfileCurve(kCurveProfile);
  ASSERT_GE(profile_curve.size(), 2U);
  ASSERT_NE(profile_curve, ClDigitalPoints());

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_TRUE(Reference().has_value());
  EXPECT_EQ(Reference()->profile_name, kCurveProfile);
  EXPECT_EQ(RgbCurve(), profile_curve);
  EXPECT_FLOAT_EQ(GradeValue("contrast"), 0.0f);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FALSE(Reference().has_value());
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
  EXPECT_FALSE(Head().has_value());

  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  ASSERT_TRUE(Reference().has_value());
  EXPECT_EQ(RgbCurve(), profile_curve);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, DocumentImportedBeforeFilmLikeResetsContrastFifteen) {
  // The RAW import look before #891: contrast 15 and the identity curve.
  auto  root_document = CreateDefaultPipelineDocument();
  auto* grade         = root_document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->FindAdjustmentByType(type_ids::Contrast())
      ->LoadJson({{"contrast", kImportLookContrastBeforeFilmLike}});
  dynamic_cast<CurveModel*>(grade->FindAdjustmentByType(type_ids::Curve()))
      ->SetPoints(IdentityCurvePoints());
  lease_ = test::MakeInMemoryEditorLease(42, std::move(root_document));
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_FLOAT_EQ(GradeValue("contrast"), kImportLookContrastBeforeFilmLike);

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("contrast"), kProfileToneEntryContrast);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FLOAT_EQ(GradeValue("contrast"), kImportLookContrastBeforeFilmLike);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
  EXPECT_EQ(RgbCurve(), IdentityCurvePoints());
}

TEST_F(EditorProfileToneEntryHistoryTest, FilmPrintEntryResetsAnEarlierFilmLikeCurve) {
  // The RAW import look between #891 and #899: the Film-like points before the highlight shoulder.
  // ToneCurvePresetTest checks that IsClDigitalToneCurve knows each earlier point list.
  const auto                    before_shoulder = ClDigitalToneCurveEarlierPoints().front();
  const std::vector<CurvePoint> stored{before_shoulder.begin(), before_shoulder.end()};
  ASSERT_NE(stored, ClDigitalPoints());
  auto  root_document = CreateDefaultPipelineDocument();
  auto* grade         = root_document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  dynamic_cast<CurveModel*>(grade->FindAdjustmentByType(type_ids::Curve()))->SetPoints(stored);
  lease_ = test::MakeInMemoryEditorLease(42, std::move(root_document));
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_EQ(RgbCurve(), stored);

  ASSERT_TRUE(CommitMethod(handle, "film_print"));
  EXPECT_EQ(RgbCurve(), IdentityCurvePoints());
}

TEST_F(EditorProfileToneEntryHistoryTest, DcpSelectionOnAces20LoadsTheAces20Fit) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitMethod(handle, "aces_2_0"));
  ASSERT_EQ(Working()->Document().Drt()->Params().Method(), DrtMethod::Aces20);

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::Aces20));
  EXPECT_NE(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::OpenDrt));
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, ProfileCurveEntryKeepsExposure) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(Commit(handle, WithColorGradeTarget({"exposure", R"({"exposure_ev":2.0})", true})));
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
  nlohmann::json json;
  ASSERT_TRUE(ReadEditorParameterJson(Working()->Document(), ColorGradeFieldTarget("exposure"),
                                      &json, &error))
      << error;
  EXPECT_FLOAT_EQ(json.at("exposure_ev").get<float>(), 2.0f);
}

TEST_F(EditorProfileToneEntryHistoryTest, ProfileCurveEntryReplacesAnEditedCurveAndKeepsContrast) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitGrade(handle, "contrast", 30.0f));
  ASSERT_TRUE(CommitCurvePreset(handle, "medium_contrast"));
  const auto medium = RgbCurve();

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("contrast"), 30.0f);
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FALSE(Reference().has_value());
  EXPECT_FLOAT_EQ(GradeValue("contrast"), 30.0f);
  EXPECT_EQ(RgbCurve(), medium);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, DocumentWithoutColorGradeOneKeepsItsValues) {
  auto root_document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(RemoveColorGradeAndBridge(root_document, NodeId{"grade.primary"}).empty());
  ASSERT_EQ(root_document.PrimaryGrade(), nullptr);
  lease_ = test::MakeInMemoryEditorLease(42, std::move(root_document));
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before = Working()->Document().ToJson().dump();

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_TRUE(Reference().has_value());
  EXPECT_TRUE(Reference()->profile_tone);
  EXPECT_EQ(Working()->Document().PrimaryGrade(), nullptr);
  // The profile curve entered, but no Grade holds a curve to write.
  EXPECT_TRUE(
      ProfileToneCurveWrites(Working()->Document(), std::nullopt, DrtMethod::OpenDrt).empty());

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(Working()->Document().ToJson().dump(), before);
}

TEST_F(EditorProfileToneEntryHistoryTest, FilmPrintFollowsNoProfileCurveAndItsExitLoadsIt) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitMethod(handle, "film_print"));
  // The Film Print entry reset the saturation and the Film-like curve.
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kFilmPrintEntrySaturation);
  EXPECT_EQ(RgbCurve(), IdentityCurvePoints());
  // The user selects the values of the import look again.
  ASSERT_TRUE(CommitGrade(handle, "saturation", kDefaultPipelineSaturation));
  ASSERT_TRUE(CommitCurvePreset(handle, "cl_digital"));

  // On Film Print the RGB curve follows no profile curve.
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_TRUE(Reference().has_value());
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);

  // Leaving Film Print starts the profile curve: an entry.
  ASSERT_TRUE(CommitMethod(handle, "open_drt"));
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, MethodChangeMovesTheProfileCurveAndFilmPrintEndsIt) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_EQ(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::OpenDrt));

  ASSERT_TRUE(CommitMethod(handle, "aces_2_0"));
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::Aces20));
  // Film Print gives the tone of its negative and print: the curve goes to the identity.
  ASSERT_TRUE(CommitMethod(handle, "film_print"));
  EXPECT_EQ(RgbCurve(), IdentityCurvePoints());
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::Aces20));
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile, DrtMethod::OpenDrt));
}

TEST_F(EditorProfileToneEntryHistoryTest, EditedCurveStaysOnMethodChangeAndOnSwitchOff) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_TRUE(CommitCurvePreset(handle, "medium_contrast"));
  const auto medium = RgbCurve();

  ASSERT_TRUE(CommitMethod(handle, "aces_2_0"));
  EXPECT_EQ(RgbCurve(), medium);
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile, false));
  EXPECT_FALSE(Reference()->profile_tone);
  EXPECT_EQ(RgbCurve(), medium);
}

TEST_F(EditorProfileToneEntryHistoryTest, ToneCurveSwitchOffPutsTheImportLookBack) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile, false));
  ASSERT_TRUE(Reference().has_value());
  EXPECT_FALSE(Reference()->profile_tone);
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  // The entry values stay: only the curve goes back.
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
}

TEST_F(EditorProfileToneEntryHistoryTest, ToneCurveSwitchOnIsAnEntry) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  // A selection with the switch off follows no profile curve: no write.
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile, false));
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
  // An edited curve: the switch loads the profile curve all the same.
  ASSERT_TRUE(CommitCurvePreset(handle, "strong_contrast"));
  const auto strong = RgbCurve();

  ASSERT_TRUE(CommitProfile(handle, kCurveProfile, true));
  EXPECT_TRUE(Reference()->profile_tone);
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);

  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FALSE(Reference()->profile_tone);
  EXPECT_EQ(RgbCurve(), strong);
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, ProfileChangeMovesAnUneditedCurveOnly) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_NE(ProfileCurve(kCurveProfile), ProfileCurve(kOtherCurveProfile));
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));

  ASSERT_TRUE(CommitProfile(handle, kOtherCurveProfile));
  EXPECT_EQ(Reference()->profile_name, kOtherCurveProfile);
  EXPECT_EQ(RgbCurve(), ProfileCurve(kOtherCurveProfile));

  ASSERT_TRUE(CommitCurvePreset(handle, "cl_digital"));
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
}

TEST_F(EditorProfileToneEntryHistoryTest, ProfileWithoutCurveWritesNoGradeValueAndLaterCurveDoes) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitProfile(handle, kFlatProfile));
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kDefaultPipelineSaturation);

  // From a profile without a curve to one with a curve: an entry.
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  EXPECT_EQ(RgbCurve(), ProfileCurve(kCurveProfile));
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
}

TEST_F(EditorProfileToneEntryHistoryTest, LeaveToDefaultEntryPutsTheImportLookCurveBack) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitProfile(handle, kCurveProfile));
  ASSERT_TRUE(CommitGrade(handle, "contrast", 40.0f));

  ASSERT_TRUE(Commit(handle, PatchFromJson("camera_profile_file", "null", true)));
  EXPECT_FALSE(Reference().has_value());
  EXPECT_FLOAT_EQ(GradeValue("contrast"), 40.0f);
  EXPECT_EQ(RgbCurve(), ClDigitalPoints());
  EXPECT_FLOAT_EQ(GradeValue("saturation"), kProfileToneEntrySaturation);
}

}  // namespace
}  // namespace alcedo::ui
