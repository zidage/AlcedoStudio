//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6D unit tests for Look typed models: white balance, HSL, CDL trackball,
// and LUT catalog. Asserts operator-shaped params JSON, interactive + one
// settled commit per completed drag, load-only setters, and the canEdit check.
// No QML / GPU.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QVariantList>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "app/editor_panel_projection.hpp"
#include "app/lut_library_inventory.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "json.hpp"
#include "support/recording_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
#include "ui/alcedo_main/album_backend/editor_grade_white_balance_model.hpp"
#include "ui/alcedo_main/album_backend/editor_hls_model.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"
#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"
#include "ui/alcedo_main/editor_support/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_support/modules/hls.hpp"

namespace alcedo::ui::test {
namespace {

auto ParseObject(const QString& params) -> QJsonObject {
  return QJsonDocument::fromJson(params.toUtf8()).object();
}

class FixedRootPreferences final : public alcedo::LutLibraryPreferences {
 public:
  [[nodiscard]] auto LoadRoot() const -> std::optional<std::filesystem::path> override {
    return std::nullopt;
  }
  [[nodiscard]] auto SaveRoot(const std::filesystem::path&) -> bool override { return true; }
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override { return {}; }
  void               SaveLegacyFavoritePaths(const QStringList&) override {}
};

/// A started LutLibraryService over a temporary root under the working directory.
class TemporaryLutLibrary {
 public:
  explicit TemporaryLutLibrary(bool open_succeeds = true, bool official_package = false) {
    std::random_device device;
    root_ =
        std::filesystem::current_path() / "editor_look_model_test_luts" / std::to_string(device());
    std::filesystem::create_directories(root_);
    root_ = std::filesystem::weakly_canonical(root_);
    Write("kodak/look.cube");
    Write("fuji/look.cube");
    if (official_package) {
      // Active content of an installed package: its receipt names the content directory.
      Write(kOfficialContent, R"(# ALCEDO_LUT {"schema":1,"id":"kodak-5207","origin":"alcedo",)"
                              R"("category":"general","input_space":"ACEScc",)"
                              R"("output_space":"ACEScc"})"
                              "\n");
      alcedo::LutPackageReceipt receipt;
      receipt.package_id        = "spectral_film_lut";
      receipt.content_directory = "packages/spectral_film_lut/content/a";
      EXPECT_TRUE(alcedo::WriteLutPackageReceiptFile(root_, receipt).empty());
    }
    alcedo::LutLibraryServiceOptions options;
    options.preferences  = std::make_unique<FixedRootPreferences>();
    options.default_root = root_;
    options.open_url     = [open_succeeds](const QUrl&) { return open_succeeds; };
    service_             = std::make_unique<alcedo::LutLibraryService>(std::move(options));
    service_->Start();
    QElapsedTimer timer;
    timer.start();
    while (service_->busy() && timer.elapsed() < 20000) QTest::qWait(5);
  }
  ~TemporaryLutLibrary() {
    service_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  TemporaryLutLibrary(const TemporaryLutLibrary&)            = delete;
  TemporaryLutLibrary& operator=(const TemporaryLutLibrary&) = delete;

  [[nodiscard]] auto   Service() -> alcedo::LutLibraryService* { return service_.get(); }
  [[nodiscard]] auto   PathOf(const char* relative) const -> QString {
    return QString::fromStdString(alcedo::LutPathToUtf8(root_ / relative));
  }

  static constexpr const char* kOfficialContent =
      "packages/spectral_film_lut/content/a/kodak-5207.cube";

 private:
  void Write(const char* relative, const char* header = "") const {
    const std::filesystem::path path = root_ / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << header << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  }

  std::filesystem::path                      root_;
  std::unique_ptr<alcedo::LutLibraryService> service_;
};

auto EntryPaths(const EditorLutCatalogModel& model) -> QStringList {
  QStringList paths;
  for (const QVariant& entry : model.entries()) {
    paths.push_back(entry.toMap().value(QStringLiteral("path")).toString());
  }
  return paths;
}

}  // namespace

// ── Color temperature ───────────────────────────────────────────────────────

TEST(EditorLookModelTest, ColorTempDefaultParamsMatchOperatorShape) {
  RecordingSubmitter   sub;
  EditorColorTempModel model;
  model.setSubmitter(&sub);

  const auto root = ParseObject(model.paramsJson());
  ASSERT_TRUE(root.contains(QStringLiteral("color_temp")));
  const auto ct = root.value(QStringLiteral("color_temp")).toObject();
  EXPECT_EQ(ct.value(QStringLiteral("mode")).toString(), QStringLiteral("as_shot"));
  EXPECT_TRUE(ct.contains(QStringLiteral("custom_cct")));
  EXPECT_TRUE(ct.contains(QStringLiteral("custom_tint")));
  EXPECT_TRUE(ct.contains(QStringLiteral("as_shot_cct")));
  EXPECT_TRUE(ct.contains(QStringLiteral("as_shot_tint")));
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, ColorTempCctDragPromotesToCustomAndSettlesOnce) {
  RecordingSubmitter   sub;
  EditorColorTempModel model;
  model.setSubmitter(&sub);
  model.setAsShotCct(5600.0);
  model.setAsShotTint(5.0);
  model.loadFromParams(QStringLiteral("as_shot"), 5600.0, 5.0, true);

  model.beginCctDrag();
  model.updateCctDrag(6200.0);
  model.updateCctDrag(6400.0);
  model.finishCctDrag();

  EXPECT_EQ(model.modeIndex(), 1);
  EXPECT_EQ(model.modeValue(), QStringLiteral("custom"));
  EXPECT_NEAR(model.cct(), 6400.0, 1e-3);
  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);

  ASSERT_NE(sub.lastSettledWrite(), nullptr);
  const auto* update =
      std::get_if<alcedo::DevelopColorTemperatureUpdate>(sub.lastSettledWrite());
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->wb_mode.has_value());
  EXPECT_EQ(*update->wb_mode, "custom");
  ASSERT_TRUE(update->custom_cct.has_value());
  EXPECT_NEAR(*update->custom_cct, 6400.0, 1e-3);
  ASSERT_TRUE(update->as_shot_cct.has_value());
  EXPECT_NEAR(*update->as_shot_cct, 5600.0, 1e-3);
}

TEST(EditorLookModelTest, ColorTempLoadFromOperatorParamsUsesGetParamsKeys) {
  RecordingSubmitter   sub;
  EditorColorTempModel model;
  model.setSubmitter(&sub);

  QVariantMap inner;
  inner.insert(QStringLiteral("mode"), QStringLiteral("as_shot"));
  inner.insert(QStringLiteral("custom_cct"), 4500.0);
  inner.insert(QStringLiteral("custom_tint"), 20.0);
  inner.insert(QStringLiteral("as_shot_cct"), 5200.0);
  inner.insert(QStringLiteral("as_shot_tint"), -8.0);
  QVariantMap root;
  root.insert(QStringLiteral("color_temp"), inner);

  model.loadFromOperatorParams(root);
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model.modeIndex(), 0);
  EXPECT_NEAR(model.cct(), 5200.0, 1e-3);
  EXPECT_NEAR(model.tint(), -8.0, 1e-3);
  EXPECT_NEAR(model.asShotCct(), 5200.0, 1e-3);
  EXPECT_NEAR(model.asShotTint(), -8.0, 1e-3);

  // Custom mode must show custom_* while keeping as-shot baseline.
  inner.insert(QStringLiteral("mode"), QStringLiteral("custom"));
  root.insert(QStringLiteral("color_temp"), inner);
  model.loadFromOperatorParams(root);
  EXPECT_EQ(model.modeIndex(), 1);
  EXPECT_NEAR(model.cct(), 4500.0, 1e-3);
  EXPECT_NEAR(model.tint(), 20.0, 1e-3);
  EXPECT_NEAR(model.asShotCct(), 5200.0, 1e-3);
  EXPECT_NEAR(model.asShotTint(), -8.0, 1e-3);

  // Switching back to as_shot without moving sliders must re-display as-shot.
  model.selectMode(0);
  EXPECT_NEAR(model.cct(), 5200.0, 1e-3);
  EXPECT_NEAR(model.tint(), -8.0, 1e-3);
}

TEST(EditorLookModelTest, ColorTempResetRestoresAsShotAndCommits) {
  RecordingSubmitter   sub;
  EditorColorTempModel model;
  model.setSubmitter(&sub);
  model.setAsShotCct(5000.0);
  model.setAsShotTint(-10.0);
  model.selectMode(1);
  model.editCct(7000.0);
  sub.calls.clear();

  model.reset();
  EXPECT_EQ(model.modeIndex(), 0);
  EXPECT_NEAR(model.cct(), 5000.0, 1e-3);
  EXPECT_NEAR(model.tint(), -10.0, 1e-3);
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorLookModelTest, ColorTempLoadOnlyDoesNotSubmit) {
  RecordingSubmitter   sub;
  EditorColorTempModel model;
  model.setSubmitter(&sub);
  model.loadFromParams(QStringLiteral("custom"), 4500.0, 12.0, true);
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model.modeIndex(), 1);
  EXPECT_NEAR(model.cct(), 4500.0, 1e-3);
}

TEST(EditorLookModelTest, ColorTempCanEditFalseDropsSubmits) {
  RecordingSubmitter sub;
  sub.canEditState = false;
  EditorColorTempModel model;
  model.setSubmitter(&sub);
  model.beginCctDrag();
  model.updateCctDrag(6000.0);
  model.finishCctDrag();
  EXPECT_TRUE(sub.calls.empty());
}

// ── Grade white balance (CAT02) ─────────────────────────────────────────────

TEST(EditorLookModelTest, GradeWhiteBalanceDefaultsToAp1WhiteWithoutSubmitting) {
  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);
  EXPECT_EQ(model.fieldKey(), QStringLiteral("grade_white_balance"));
  EXPECT_NEAR(model.temperature(), alcedo::kCat02DefaultTemperature, 1e-3);
  EXPECT_NEAR(model.tint(), alcedo::kCat02DefaultTint, 1e-3);
  EXPECT_EQ(model.temperatureSliderPos(), color_temp::kSliderUiMid);
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, GradeWhiteBalanceTemperatureDragSubmitsCat02UpdateAndSettlesOnce) {
  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);

  model.beginTemperatureDrag();
  model.updateTemperatureSliderDrag(color_temp::CctToSliderPos(5000.0f));
  model.updateTemperatureSliderDrag(color_temp::CctToSliderPos(4500.0f));
  model.finishTemperatureDrag();

  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
  ASSERT_NE(sub.lastSettled(), nullptr);
  EXPECT_EQ(sub.lastSettled()->fieldKey, QStringLiteral("grade_white_balance"));
  const auto* update = std::get_if<alcedo::Cat02WhiteBalanceUpdate>(sub.lastSettledWrite());
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->temperature.has_value());
  EXPECT_NEAR(*update->temperature, model.temperature(), 1e-3);
  EXPECT_NEAR(*update->temperature, 4500.0, 25.0);
  ASSERT_TRUE(update->tint.has_value());
  EXPECT_NEAR(*update->tint, alcedo::kCat02DefaultTint, 1e-3);
  EXPECT_FALSE(update->enabled.has_value());
}

TEST(EditorLookModelTest, GradeWhiteBalanceEmptyClickDoesNotSettle) {
  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);
  model.beginTintDrag();
  model.finishTintDrag();
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, GradeWhiteBalanceResetRestoresAp1WhitePerSlider) {
  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);
  model.setTemperature(3500.0);
  model.setTint(40.0);

  model.resetTemperature();
  EXPECT_NEAR(model.temperature(), alcedo::kCat02DefaultTemperature, 1e-3);
  EXPECT_NEAR(model.tint(), 40.0, 1e-3);
  EXPECT_EQ(sub.settledCount(), 1);

  model.resetTint();
  EXPECT_NEAR(model.tint(), alcedo::kCat02DefaultTint, 1e-3);
  EXPECT_EQ(sub.settledCount(), 2);

  // Already at default: no extra history commit.
  model.resetTint();
  EXPECT_EQ(sub.settledCount(), 2);
}

TEST(EditorLookModelTest, GradeWhiteBalanceLoadsProjectionWithoutSubmitting) {
  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);
  QVariantMap inner;
  inner.insert(QStringLiteral("temperature"), 5200.0);
  inner.insert(QStringLiteral("tint"), -6.0);
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("grade_white_balance"), inner);

  model.loadFromSnapshot(snapshot);
  EXPECT_NEAR(model.temperature(), 5200.0, 1e-3);
  EXPECT_NEAR(model.tint(), -6.0, 1e-3);
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, GradeWhiteBalanceLoadsStoredValuesFromPanelSnapshotOnSourceSwitch) {
  auto  document = alcedo::CreateDefaultPipelineDocument();
  auto* cat02    = dynamic_cast<alcedo::Cat02WhiteBalanceModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(alcedo::type_ids::Cat02WhiteBalance()));
  ASSERT_NE(cat02, nullptr);
  cat02->ApplyUpdate(alcedo::Cat02WhiteBalanceUpdate{std::nullopt, 4100.0f, -22.0f});

  alcedo::EditorPanelProjection projection;
  std::string                   error;
  ASSERT_TRUE(alcedo::ProjectCurrentPanelFields(document, 1, &projection, &error)) << error;
  const auto snapshot = PanelProjectionToVariantMap(projection);

  RecordingSubmitter           sub;
  EditorGradeWhiteBalanceModel model;
  model.setSubmitter(&sub);
  model.setTemperature(7300.0);
  model.setTint(35.0);

  // The editor passes the whole adjustment snapshot when the image, layer, or mask changes.
  model.loadFromSnapshot(snapshot);
  EXPECT_NEAR(model.temperature(), 4100.0, 1e-3);
  EXPECT_NEAR(model.tint(), -22.0, 1e-3);
  EXPECT_TRUE(sub.calls.empty());
}

// ── HSL ─────────────────────────────────────────────────────────────────────

TEST(EditorLookModelTest, HlsDefaultParamsMatchOperatorShape) {
  RecordingSubmitter sub;
  EditorHlsModel     model;
  model.setSubmitter(&sub);

  const auto root = ParseObject(model.paramsJson());
  ASSERT_TRUE(root.contains(QStringLiteral("HLS")));
  const auto hls = root.value(QStringLiteral("HLS")).toObject();
  EXPECT_EQ(hls.value(QStringLiteral("hue_bins")).toArray().size(),
            static_cast<int>(hls::kCandidateHues.size()));
  EXPECT_EQ(hls.value(QStringLiteral("hls_adj_table")).toArray().size(),
            static_cast<int>(hls::kCandidateHues.size()));
  EXPECT_TRUE(hls.contains(QStringLiteral("target_hls")));
  EXPECT_TRUE(hls.contains(QStringLiteral("hls_adj")));
  EXPECT_TRUE(hls.contains(QStringLiteral("h_range")));
}

TEST(EditorLookModelTest, HlsHueSwatchSwitchDoesNotSubmit) {
  RecordingSubmitter sub;
  EditorHlsModel     model;
  model.setSubmitter(&sub);
  model.beginHueShiftDrag();
  model.updateHueShiftDrag(10.0);
  model.finishHueShiftDrag();
  sub.calls.clear();

  model.selectHueIndex(2);
  EXPECT_EQ(model.activeHueIndex(), 2);
  EXPECT_NEAR(model.hueShift(), 0.0, 1e-6);
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, HlsProfilePersistsAcrossHueSwitch) {
  RecordingSubmitter sub;
  EditorHlsModel     model;
  model.setSubmitter(&sub);

  model.beginChromaDrag();
  model.updateChromaDrag(40.0);
  model.finishChromaDrag();
  model.selectHueIndex(3);
  model.selectHueIndex(0);
  EXPECT_NEAR(model.chroma(), 40.0, 1e-6);
}

TEST(EditorLookModelTest, HlsDragSubmitsInteractiveThenOneSettled) {
  RecordingSubmitter sub;
  EditorHlsModel     model;
  model.setSubmitter(&sub);

  model.beginLightnessDrag();
  model.updateLightnessDrag(20.0);
  model.updateLightnessDrag(30.0);
  model.finishLightnessDrag();

  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(sub.calls.back().fieldKey, QStringLiteral("hls"));

  ASSERT_NE(sub.lastSettledWrite(), nullptr);
  const auto* update = std::get_if<alcedo::HlsUpdate>(sub.lastSettledWrite());
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->hls_adj.has_value());
  EXPECT_NEAR(update->hls_adj->l, 30.0f / hls::kAdjUiToParamScale, 1e-6);
}

// ── CDL trackball ───────────────────────────────────────────────────────────

TEST(EditorLookModelTest, CdlDefaultParamsMatchOperatorShape) {
  RecordingSubmitter      sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);

  const auto root = ParseObject(model.paramsJson());
  ASSERT_TRUE(root.contains(QStringLiteral("color_wheel")));
  const auto cw = root.value(QStringLiteral("color_wheel")).toObject();
  for (const char* key : {"lift", "gamma", "gain"}) {
    ASSERT_TRUE(cw.contains(QString::fromUtf8(key))) << key;
    const auto wheel = cw.value(QString::fromUtf8(key)).toObject();
    EXPECT_TRUE(wheel.contains(QStringLiteral("disc")));
    EXPECT_TRUE(wheel.contains(QStringLiteral("color_offset")));
    EXPECT_TRUE(wheel.contains(QStringLiteral("luminance_offset")));
    EXPECT_TRUE(wheel.contains(QStringLiteral("strength")));
  }
}

TEST(EditorLookModelTest, CdlDiscDragInteractiveThenOneSettled) {
  RecordingSubmitter      sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);

  model.beginDiscDrag(QStringLiteral("lift"));
  model.updateDiscDrag(QStringLiteral("lift"), 0.2, 0.1);
  model.updateDiscDrag(QStringLiteral("lift"), 0.4, 0.2);
  model.finishDiscDrag();

  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_NEAR(model.liftX(), 0.4, 1e-3);
  EXPECT_NEAR(model.liftY(), 0.2, 1e-3);

  ASSERT_NE(sub.lastSettledWrite(), nullptr);
  const auto* update = std::get_if<alcedo::ColorWheelUpdate>(sub.lastSettledWrite());
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->lift.has_value());
  ASSERT_TRUE(update->lift->disc.has_value());
  EXPECT_NEAR(update->lift->disc->x, 0.4, 1e-3);
}

TEST(EditorLookModelTest, CdlGammaMasterUiIsInverted) {
  RecordingSubmitter      sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);

  model.beginMasterDrag(QStringLiteral("gamma"));
  // Positive UI drag should store negative gamma master (legacy invert).
  model.updateMasterDragUi(QStringLiteral("gamma"), 400);
  model.finishMasterDrag();

  EXPECT_LT(model.gammaMaster(), 0.0);
  EXPECT_EQ(model.gammaMasterUi(), 400);
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorLookModelTest, CdlResetWheelSettlesOnce) {
  RecordingSubmitter      sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);
  model.beginDiscDrag(QStringLiteral("gain"));
  model.updateDiscDrag(QStringLiteral("gain"), 0.5, -0.2);
  model.finishDiscDrag();
  sub.calls.clear();

  model.resetWheel(QStringLiteral("gain"));
  EXPECT_NEAR(model.gainX(), 0.0, 1e-6);
  EXPECT_NEAR(model.gainY(), 0.0, 1e-6);
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorLookModelTest, CdlLoadOnlyDoesNotSubmit) {
  RecordingSubmitter      sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);
  model.setWheelDisc(QStringLiteral("lift"), 0.1, 0.2);
  model.setWheelMaster(QStringLiteral("lift"), 0.05);
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_NEAR(model.liftX(), 0.1, 1e-6);
}

// ── LUT catalog ─────────────────────────────────────────────────────────────

TEST(EditorLookModelTest, LutSelectPathSubmitsSelectionAndKeepsStrength) {
  RecordingSubmitter    sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);

  model.selectPath(QStringLiteral("D:/fake/look.cube"));
  EXPECT_EQ(sub.settledCount(), 1);
  ASSERT_NE(sub.lastSettledWrite(), nullptr);
  const auto* update = std::get_if<alcedo::EditorLutWrite>(sub.lastSettledWrite());
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->reference.has_value());
  EXPECT_EQ(*update->reference,
            alcedo::LutReference{alcedo::FileLutReference{"D:/fake/look.cube"}});
  // A selection does not carry a strength, so the configured strength is kept.
  EXPECT_FALSE(update->strength.has_value());
}

TEST(EditorLookModelTest, LutSelectionOfPackageFileSubmitsOfficialReference) {
  TemporaryLutLibrary   library(true, true);
  RecordingSubmitter    sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);
  model.setLibrary(library.Service());

  model.selectPath(library.PathOf(TemporaryLutLibrary::kOfficialContent));
  const auto* official = std::get_if<alcedo::EditorLutWrite>(sub.lastSettledWrite());
  ASSERT_NE(official, nullptr);
  ASSERT_TRUE(official->reference.has_value());
  EXPECT_EQ(
      *official->reference,
      (alcedo::LutReference{alcedo::OfficialLutReference{"spectral_film_lut", "kodak-5207"}}));
  EXPECT_FALSE(official->display_name.empty());
  EXPECT_FALSE(official->strength.has_value());

  model.selectPath(library.PathOf("kodak/look.cube"));
  const auto* loose = std::get_if<alcedo::EditorLutWrite>(sub.lastSettledWrite());
  ASSERT_NE(loose, nullptr);
  EXPECT_EQ(*loose->reference,
            alcedo::LutReference{alcedo::LibraryLutReference{"kodak/look.cube"}});
}

TEST(EditorLookModelTest, LutLoadSelectionResolvesReferencesWithoutSubmitting) {
  TemporaryLutLibrary   library(true, true);
  RecordingSubmitter    sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);
  model.setLibrary(library.Service());

  model.loadSelection(
      QVariantMap{{QStringLiteral("referenceKind"), QStringLiteral("official")},
                  {QStringLiteral("packageId"), QStringLiteral("spectral_film_lut")},
                  {QStringLiteral("lutId"), QStringLiteral("kodak-5207")},
                  {QStringLiteral("lutName"), QStringLiteral("Vision3 250D")}});
  EXPECT_EQ(model.selectedPath(), library.PathOf(TemporaryLutLibrary::kOfficialContent));
  EXPECT_GT(model.selectedIndex(), 0);

  model.loadSelection(
      QVariantMap{{QStringLiteral("referenceKind"), QStringLiteral("library")},
                  {QStringLiteral("libraryPath"), QStringLiteral("fuji/look.cube")}});
  EXPECT_EQ(model.selectedPath(), library.PathOf("fuji/look.cube"));

  // An official LUT that no installed package lists stays visible as missing, by its name.
  model.loadSelection(
      QVariantMap{{QStringLiteral("referenceKind"), QStringLiteral("official")},
                  {QStringLiteral("packageId"), QStringLiteral("spectral_film_lut")},
                  {QStringLiteral("lutId"), QStringLiteral("retired-stock")},
                  {QStringLiteral("lutName"), QStringLiteral("Retired Stock")}});
  ASSERT_EQ(model.selectedIndex(), 1);
  const auto row = model.entries()[1].toMap();
  EXPECT_EQ(row.value(QStringLiteral("kind")).toString(), QStringLiteral("missing"));
  EXPECT_EQ(row.value(QStringLiteral("displayName")).toString(), QStringLiteral("Retired Stock"));
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorLookModelTest, LutSetSelectedPathIsLoadOnly) {
  RecordingSubmitter    sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);
  model.setSelectedPath(QStringLiteral("D:/fake/load_only.cube"));
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model.selectedPath(), QStringLiteral("D:/fake/load_only.cube"));
}

TEST(EditorLookModelTest, LutClearSelectionCommitsEmptyPath) {
  RecordingSubmitter    sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);
  model.setSelectedPath(QStringLiteral("D:/fake/a.cube"));
  model.clearSelection();
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_TRUE(model.selectedPath().isEmpty());
}

TEST(EditorLookModelTest, LutSelectPathDoesNotEmitEntriesChanged) {
  // Selection must not rebuild the catalog entry list. Emitting entriesChanged
  // on every click forces QML ListViews to reset contentY and pin the selected
  // row to the bottom of the viewport.
  EditorLutCatalogModel model;
  QSignalSpy            entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  QSignalSpy            selected_spy(&model, &EditorLutCatalogModel::selectedPathChanged);
  ASSERT_TRUE(entries_spy.isValid());
  ASSERT_TRUE(selected_spy.isValid());

  model.selectPath(QStringLiteral("D:/fake/look.cube"));
  EXPECT_EQ(selected_spy.count(), 1);
  EXPECT_EQ(entries_spy.count(), 0);
  EXPECT_EQ(model.selectedPath(), QStringLiteral("D:/fake/look.cube"));

  // Second select of a different path still must not rebuild entries.
  model.selectPath(QStringLiteral("D:/fake/other.cube"));
  EXPECT_EQ(selected_spy.count(), 2);
  EXPECT_EQ(entries_spy.count(), 0);
}

TEST(EditorLookModelTest, LutSetSelectedPathDoesNotEmitEntriesChanged) {
  EditorLutCatalogModel model;
  QSignalSpy            entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  model.setSelectedPath(QStringLiteral("D:/fake/load_only.cube"));
  EXPECT_EQ(entries_spy.count(), 0);
  EXPECT_EQ(model.selectedPath(), QStringLiteral("D:/fake/load_only.cube"));
}

TEST(EditorLookModelTest, LutFavoriteToggleStoresEntryInLibrary) {
  TemporaryLutLibrary   library;
  EditorLutCatalogModel model;
  model.setLibrary(library.Service());
  const QString path = library.PathOf("kodak/look.cube");
  EXPECT_FALSE(model.isFavoritePath(path));
  EXPECT_FALSE(model.isFavoritePath(QString()));
  EXPECT_FALSE(model.isFavoritePath(QStringLiteral("   ")));

  QSignalSpy fav_spy(&model, &EditorLutCatalogModel::favoritePathsChanged);
  ASSERT_TRUE(fav_spy.isValid());

  model.toggleFavoritePath(path);
  EXPECT_EQ(fav_spy.count(), 1);
  EXPECT_TRUE(model.isFavoritePath(path));
  EXPECT_FALSE(model.isFavoritePath(library.PathOf("fuji/look.cube")));
  EXPECT_EQ(model.favoritePaths(), QStringList{path});
  EXPECT_EQ(library.Service()->FavoritePaths(), std::vector<std::string>{"kodak/look.cube"});

  model.toggleFavoritePath(path);
  EXPECT_EQ(fav_spy.count(), 2);
  EXPECT_FALSE(model.isFavoritePath(path));
  EXPECT_TRUE(model.favoritePaths().isEmpty());

  // A path outside the library cannot become a favorite.
  model.toggleFavoritePath(QStringLiteral("D:/fake/favorite.cube"));
  EXPECT_EQ(fav_spy.count(), 2);
}

TEST(EditorLookModelTest, LutCatalogListsLibraryEntriesWithoutBasenameMatching) {
  TemporaryLutLibrary   library;
  EditorLutCatalogModel model;
  model.setLibrary(library.Service());
  EXPECT_EQ(EntryPaths(model), (QStringList{QString(), library.PathOf("fuji/look.cube"),
                                            library.PathOf("kodak/look.cube")}));

  model.setSelectedPath(library.PathOf("kodak/look.cube"));
  EXPECT_EQ(model.selectedIndex(), 2);

  // A missing file with the same name in another folder selects neither listed file.
  model.setSelectedPath(library.PathOf("agfa/look.cube"));
  model.refresh(false);
  ASSERT_EQ(model.selectedIndex(), 1);
  EXPECT_EQ(model.entries()[1].toMap().value(QStringLiteral("kind")).toString(),
            QStringLiteral("missing"));
  EXPECT_EQ(model.entries().size(), 4);
}

TEST(EditorLookModelTest, LutOpenDirectoryFailureIsShownInStatusText) {
  TemporaryLutLibrary   library(false);
  EditorLutCatalogModel model;
  model.setLibrary(library.Service());
  QSignalSpy failed_spy(&model, &EditorLutCatalogModel::openFolderFailed);

  EXPECT_FALSE(model.openDirectory());
  ASSERT_EQ(failed_spy.count(), 1);
  EXPECT_FALSE(model.statusText().isEmpty());
  EXPECT_EQ(model.statusText(), failed_spy.at(0).at(0).toString());
  EXPECT_TRUE(model.statusText().contains(library.Service()->root_path()));
}

TEST(EditorLookModelTest, LutFilterRebuildsEntriesAndEmitsEntriesChanged) {
  EditorLutCatalogModel model;
  QSignalSpy            entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  model.setFilterText(QStringLiteral("no-match-zzzz"));
  EXPECT_GE(entries_spy.count(), 1);
  // Filter is applied; selection is independent of the filtered view size.
  model.selectPath(QStringLiteral("D:/fake/still_select.cube"));
  const int after_filter = entries_spy.count();
  model.selectPath(QStringLiteral("D:/fake/still_select_2.cube"));
  EXPECT_EQ(entries_spy.count(), after_filter);
}

TEST(EditorLookModelTest, HlsModelLoadFromTablesRestoresUiValues) {
  // Simulate user editing: select hue swatch 2, set lightness to 45 via drag,
  // then recreate the model from the submitted params (as happens on reopen).

  RecordingSubmitter sub;
  EditorHlsModel     model;
  model.setSubmitter(&sub);

  // Select hue swatch 2 (candidate hue ≈ 90°)
  model.selectHueIndex(2);
  EXPECT_EQ(model.activeHueIndex(), 2);

  // Set lightness to 45 via drag
  model.beginLightnessDrag();
  model.updateLightnessDrag(45.0);
  model.finishLightnessDrag();
  EXPECT_NEAR(model.lightness(), 45.0, 1e-6);

  // Get the submitted params as the panel reload reads them.
  ASSERT_FALSE(sub.calls.empty());
  const auto settled = model.paramsJson();
  ASSERT_FALSE(settled.isEmpty());

  const auto json = nlohmann::json::parse(settled.toStdString());
  ASSERT_TRUE(json.contains("HLS"));

  // Build UI tables from the submitted params (as QML loadHlsFromSnapshot does)
  const auto& rt_hls = json["HLS"];
  ASSERT_TRUE(rt_hls.contains("hls_adj_table"));
  ASSERT_TRUE(rt_hls.contains("h_range_table"));

  QVariantList ui_table;
  for (const auto& row : rt_hls["hls_adj_table"]) {
    QVariantList r;
    r.append(QVariant(row[0].get<double>()));
    // Params store L/S at 1/kAdjUiToParamScale; multiply back for UI
    r.append(QVariant(row[1].get<double>() * hls::kAdjUiToParamScale));
    r.append(QVariant(row[2].get<double>() * hls::kAdjUiToParamScale));
    ui_table.append(QVariant::fromValue(r));
  }

  QVariantList range_table;
  for (const auto& v : rt_hls["h_range_table"]) {
    range_table.append(QVariant(v.get<double>()));
  }

  double target_hue = 0.0;
  if (rt_hls.contains("target_hls") && rt_hls["target_hls"].is_array() &&
      rt_hls["target_hls"].size() > 0) {
    target_hue = rt_hls["target_hls"][0].get<double>();
  }

  // Create a fresh model and load from tables (simulates panel reload)
  EditorHlsModel     loaded;
  RecordingSubmitter dummy_sub;
  loaded.setSubmitter(&dummy_sub);
  loaded.loadFromTables(ui_table, range_table, target_hue);

  // The values must be preserved
  EXPECT_NEAR(loaded.lightness(), 45.0, 1.0);
  EXPECT_EQ(loaded.activeHueIndex(), 2) << "Active hue swatch should be restored from target_hls";
}

// ── HLS snapshot rebuild integration ───────────────────────────────────────

TEST(EditorLookModelTest, HlsSnapshotRebuildPreservesUiValues) {
  // Simulate what happens when the backend rebuilds the committed snapshot
  // from pipeline state and publishes it to QML. The snapshot patches contain
  // the operator param JSON; BuildSnapshotMap converts to QVariantMap; QML
  // loadFromSnapshot parses it into model tables.

  // Step 1: Build the HLS field JSON that the panel snapshot map carries.
  // HLS operator returns {"HLS": {hls_adj_table: [[h, L/1000, C/1000], ...]}}
  const std::string hls_patch_json = R"({
    "HLS": {
      "hue_bins": [0, 45, 90, 135, 180, 225, 270, 315],
      "hls_adj_table": [
        [0, 0, 0], [0, 0, 0], [0, 0.045, 0], [0, 0, 0],
        [5, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]
      ],
      "h_range_table": [30, 30, 30, 30, 30, 30, 30, 30],
      "target_hls": [90, 0.5, 1.0],
      "hls_adj": [0, 0.045, 0],
      "h_range": 30,
      "l_range": 0.1,
      "s_range": 0.1
    }
  })";

  // BuildSnapshotMap logic: patch.params_json -> QJsonObject -> QVariantMap
  QJsonParseError   error;
  const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(hls_patch_json), &error);
  ASSERT_EQ(error.error, QJsonParseError::NoError);
  ASSERT_TRUE(doc.isObject());
  const auto obj      = doc.object();

  auto       snapshot = QVariantMap{};
  snapshot.insert(QStringLiteral("hls"), obj.toVariantMap());

  // Step 2: QML loadFromSnapshot -> loadHlsFromSnapshot
  const auto   entry     = snapshot.value(QStringLiteral("hls")).toMap();
  const auto   hls       = entry.value(QStringLiteral("HLS")).toMap();

  // Extract tables (matching loadHlsFromSnapshot logic)
  const auto   raw_table = hls.value(QStringLiteral("hls_adj_table")).toList();
  const auto   ranges    = hls.value(QStringLiteral("h_range_table")).toList();

  // Multiply L/S by 1000 (kAdjUiToParamScale) as QML does
  QVariantList ui_table;
  for (const auto& row_var : raw_table) {
    const auto row = row_var.toList();
    if (row.size() >= 3) {
      QVariantList ui_row;
      ui_row.append(row[0].toDouble());
      ui_row.append(row[1].toDouble() * hls::kAdjUiToParamScale);
      ui_row.append(row[2].toDouble() * hls::kAdjUiToParamScale);
      ui_table.append(QVariant::fromValue(ui_row));
    }
  }

  double     target_hue = 0.0;
  const auto target     = hls.value(QStringLiteral("target_hls")).toList();
  if (target.size() > 0) {
    target_hue = target[0].toDouble();
  }

  // Step 3: Load into a fresh HLS model
  RecordingSubmitter dummy;
  EditorHlsModel     loaded;
  loaded.setSubmitter(&dummy);
  loaded.loadFromTables(ui_table, ranges, target_hue);

  // Swatch index 2 has hue_bin=90 -> target_hls[0] should find it
  EXPECT_EQ(loaded.activeHueIndex(), 2);
  // That swatch had lightness=0.045 * 1000 = 45
  EXPECT_NEAR(loaded.lightness(), 45.0, 1.0);
  // Swatch 4 had hue_shift=5
  loaded.selectHueIndex(4);
  EXPECT_NEAR(loaded.hueShift(), 5.0, 1.0);
  // Swatch 2 again to confirm table persistence
  loaded.selectHueIndex(2);
  EXPECT_NEAR(loaded.lightness(), 45.0, 1.0);
  EXPECT_NEAR(loaded.chroma(), 0.0, 1.0);
}

}  // namespace alcedo::ui::test
