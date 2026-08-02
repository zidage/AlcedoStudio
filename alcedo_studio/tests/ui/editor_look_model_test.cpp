//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6D unit tests for Look typed models: white balance, HSL, CDL trackball,
// and LUT catalog. Asserts operator-shaped params JSON, interactive + one
// settled commit per completed drag, load-only setters, and canEdit gating.
// No QML / GPU.

#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"

#include "edit/operators/color/vibrance_op.hpp"
#include "edit/operators/color/HLS_op.hpp"
#include "ui/alcedo_main/album_backend/editor_hls_model.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_dialog/modules/hls.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace alcedo::ui::test {
namespace {

class RecordingSubmitter : public QObject, public IEditorAdjustmentSubmitter {
 public:
  struct Call {
    QString fieldKey;
    QString params;
    bool    settled;
  };
  std::vector<Call> calls;
  bool              canEditState = true;

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    if (!canEditState) {
      return false;
    }
    calls.push_back({fieldKey, paramsJson, settled});
    return true;
  }
  auto canEdit() const -> bool override { return canEditState; }

  auto settledCount() const -> int {
    return static_cast<int>(
        std::count_if(calls.begin(), calls.end(), [](const Call& c) { return c.settled; }));
  }
  auto interactiveCount() const -> int {
    return static_cast<int>(
        std::count_if(calls.begin(), calls.end(), [](const Call& c) { return !c.settled; }));
  }
  auto lastSettledParams() const -> QString {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      if (it->settled) {
        return it->params;
      }
    }
    return {};
  }
};

auto ParseObject(const QString& params) -> QJsonObject {
  return QJsonDocument::fromJson(params.toUtf8()).object();
}

}  // namespace

// ── Color temperature ───────────────────────────────────────────────────────

TEST(EditorLookModelTest, ColorTempDefaultParamsMatchOperatorShape) {
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
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

  const auto ct = ParseObject(sub.lastSettledParams()).value(QStringLiteral("color_temp")).toObject();
  EXPECT_EQ(ct.value(QStringLiteral("mode")).toString(), QStringLiteral("custom"));
  EXPECT_NEAR(ct.value(QStringLiteral("custom_cct")).toDouble(), 6400.0, 1e-3);
  EXPECT_NEAR(ct.value(QStringLiteral("as_shot_cct")).toDouble(), 5600.0, 1e-3);
}

TEST(EditorLookModelTest, ColorTempLoadFromOperatorParamsUsesGetParamsKeys) {
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
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

// ── HSL ─────────────────────────────────────────────────────────────────────

TEST(EditorLookModelTest, HlsDefaultParamsMatchOperatorShape) {
  RecordingSubmitter sub;
  EditorHlsModel model;
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
  EditorHlsModel model;
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
  EditorHlsModel model;
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
  EditorHlsModel model;
  model.setSubmitter(&sub);

  model.beginLightnessDrag();
  model.updateLightnessDrag(20.0);
  model.updateLightnessDrag(30.0);
  model.finishLightnessDrag();

  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(sub.calls.back().fieldKey, QStringLiteral("hls"));

  const auto hls = ParseObject(sub.lastSettledParams()).value(QStringLiteral("HLS")).toObject();
  const auto adj = hls.value(QStringLiteral("hls_adj")).toArray();
  ASSERT_EQ(adj.size(), 3);
  // UI 30 -> operator 30/1000
  EXPECT_NEAR(adj.at(1).toDouble(), 30.0 / hls::kAdjUiToParamScale, 1e-6);
}

// ── CDL trackball ───────────────────────────────────────────────────────────

TEST(EditorLookModelTest, CdlDefaultParamsMatchOperatorShape) {
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
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

  const auto lift = ParseObject(sub.lastSettledParams())
                        .value(QStringLiteral("color_wheel"))
                        .toObject()
                        .value(QStringLiteral("lift"))
                        .toObject();
  const auto disc = lift.value(QStringLiteral("disc")).toObject();
  EXPECT_NEAR(disc.value(QStringLiteral("x")).toDouble(), 0.4, 1e-3);
}

TEST(EditorLookModelTest, CdlGammaMasterUiIsInverted) {
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
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
  RecordingSubmitter sub;
  EditorCdlTrackballModel model;
  model.setSubmitter(&sub);
  model.setWheelDisc(QStringLiteral("lift"), 0.1, 0.2);
  model.setWheelMaster(QStringLiteral("lift"), 0.05);
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_NEAR(model.liftX(), 0.1, 1e-6);
}

// ── LUT catalog ─────────────────────────────────────────────────────────────

TEST(EditorLookModelTest, LutSelectPathCommitsOcioLmtShape) {
  RecordingSubmitter sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);

  model.selectPath(QStringLiteral("D:/fake/look.cube"));
  EXPECT_EQ(sub.settledCount(), 1);
  const auto root = ParseObject(sub.lastSettledParams());
  EXPECT_EQ(root.value(QStringLiteral("ocio_lmt")).toString(),
            QStringLiteral("D:/fake/look.cube"));
}

TEST(EditorLookModelTest, LutSetSelectedPathIsLoadOnly) {
  RecordingSubmitter sub;
  EditorLutCatalogModel model;
  model.setSubmitter(&sub);
  model.setSelectedPath(QStringLiteral("D:/fake/load_only.cube"));
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model.selectedPath(), QStringLiteral("D:/fake/load_only.cube"));
}

TEST(EditorLookModelTest, LutClearSelectionCommitsEmptyPath) {
  RecordingSubmitter sub;
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
  QSignalSpy entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  QSignalSpy selected_spy(&model, &EditorLutCatalogModel::selectedPathChanged);
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
  QSignalSpy entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  model.setSelectedPath(QStringLiteral("D:/fake/load_only.cube"));
  EXPECT_EQ(entries_spy.count(), 0);
  EXPECT_EQ(model.selectedPath(), QStringLiteral("D:/fake/load_only.cube"));
}

TEST(EditorLookModelTest, LutFavoriteToggleRoundTripsInMemory) {
  EditorLutCatalogModel model;
  const QString path = QStringLiteral("D:/fake/favorite.cube");
  EXPECT_FALSE(model.isFavoritePath(path));
  EXPECT_FALSE(model.isFavoritePath(QString()));
  EXPECT_FALSE(model.isFavoritePath(QStringLiteral("   ")));

  QSignalSpy fav_spy(&model, &EditorLutCatalogModel::favoritePathsChanged);
  ASSERT_TRUE(fav_spy.isValid());

  model.toggleFavoritePath(path);
  EXPECT_EQ(fav_spy.count(), 1);
  EXPECT_TRUE(model.isFavoritePath(path));
  EXPECT_TRUE(model.favoritePaths().contains(path));

  model.toggleFavoritePath(path);
  EXPECT_EQ(fav_spy.count(), 2);
  EXPECT_FALSE(model.isFavoritePath(path));
  EXPECT_FALSE(model.favoritePaths().contains(path));
}

TEST(EditorLookModelTest, LutFilterRebuildsEntriesAndEmitsEntriesChanged) {
  EditorLutCatalogModel model;
  QSignalSpy entries_spy(&model, &EditorLutCatalogModel::entriesChanged);
  model.setFilterText(QStringLiteral("no-match-zzzz"));
  EXPECT_GE(entries_spy.count(), 1);
  // Filter is applied; selection is independent of the filtered view size.
  model.selectPath(QStringLiteral("D:/fake/still_select.cube"));
  const int after_filter = entries_spy.count();
  model.selectPath(QStringLiteral("D:/fake/still_select_2.cube"));
  EXPECT_EQ(entries_spy.count(), after_filter);
}

// ── Vibrance operator round-trip ───────────────────────────────────────────

TEST(EditorLookModelTest, VibranceSetGetParamsPreservesUiValue) {
  // Simulate user setting vibrance to 75 on the [-100, 100] UI range:
  // the submit path sends {"vibrance": 75}. Pipeline stores via SetParams
  // (divides by 100 → internal 0.75). GetParams must scale back to 75.
  const float kUiValue = 75.0f;
  alcedo::VibranceOp op;
  op.SetParams({{"vibrance", kUiValue}});

  const auto params = op.GetParams();
  ASSERT_TRUE(params.contains("vibrance"));
  // Bug: GetParams returns 0.75 without the * 100.0f scale-back.
  EXPECT_NEAR(params["vibrance"].get<float>(), kUiValue, 1e-4f);
}

// ── HLS operator round-trip ────────────────────────────────────────────────

TEST(EditorLookModelTest, HlsOperatorSetGetParamsPreservesUiValues) {
  // Round-trip HLS params through the operator: set via SetParams with the
  // same JSON shape the model submits, then read back via GetParams.
  // Operator stores L/S internally at 1/kAdjUiToParamScale; GetParams must
  // return them unchanged. The QML panel multiplies by 1000 on load.

  const auto params = nlohmann::json::parse(R"({
    "HLS": {
      "hue_bins": [0, 45, 90, 135, 180, 225, 270, 315],
      "hls_adj_table": [
        [0, 0, 0], [0, 0.02, 0], [0, 0, 0], [0, 0, 0],
        [0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]
      ],
      "h_range_table": [30, 30, 30, 30, 30, 30, 30, 30],
      "target_hls": [45, 0.5, 1.0],
      "hls_adj": [0, 0.02, 0],
      "h_range": 30,
      "l_range": 0.1,
      "s_range": 0.1
    }
  })");

  alcedo::HLSOp op;
  op.SetParams(params);

  const auto rt = op.GetParams();
  ASSERT_TRUE(rt.contains("HLS"));
  const auto& hls = rt["HLS"];

  // The table at index 1 should have lightness=0.02 (20/1000 for UI 20)
  ASSERT_TRUE(hls.contains("hls_adj_table"));
  ASSERT_TRUE(hls["hls_adj_table"].is_array());
  ASSERT_GE(hls["hls_adj_table"].size(), 2);
  const auto& row = hls["hls_adj_table"][1];
  ASSERT_TRUE(row.is_array());
  ASSERT_GE(row.size(), 3);
  EXPECT_NEAR(row[1].get<double>(), 0.02, 1e-6);
  EXPECT_NEAR(row[2].get<double>(), 0.0, 1e-6);

  // target_hls should match
  ASSERT_TRUE(hls.contains("target_hls"));
  EXPECT_NEAR(hls["target_hls"][0].get<double>(), 45.0, 1e-6);
}

TEST(EditorLookModelTest, HlsModelLoadFromTablesRestoresUiValues) {
  // Simulate user editing: select hue swatch 2, set lightness to 45 via drag,
  // then recreate the model from the submitted params (as happens on reopen).

  RecordingSubmitter sub;
  EditorHlsModel model;
  model.setSubmitter(&sub);

  // Select hue swatch 2 (candidate hue ≈ 90°)
  model.selectHueIndex(2);
  EXPECT_EQ(model.activeHueIndex(), 2);

  // Set lightness to 45 via drag
  model.beginLightnessDrag();
  model.updateLightnessDrag(45.0);
  model.finishLightnessDrag();
  EXPECT_NEAR(model.lightness(), 45.0, 1e-6);

  // Get the submitted params and re-parse as an operator would
  ASSERT_FALSE(sub.calls.empty());
  const auto settled = sub.lastSettledParams();
  ASSERT_FALSE(settled.isEmpty());

  const auto json = nlohmann::json::parse(settled.toStdString());
  ASSERT_TRUE(json.contains("HLS"));

  // Round-trip through HLSOp as the pipeline does
  alcedo::HLSOp op;
  op.SetParams(json);
  const auto rt = op.GetParams();

  // Build UI tables from the operator output (as QML loadHlsFromSnapshot does)
  const auto& rt_hls = rt["HLS"];
  ASSERT_TRUE(rt_hls.contains("hls_adj_table"));
  ASSERT_TRUE(rt_hls.contains("h_range_table"));

  QVariantList ui_table;
  for (const auto& row : rt_hls["hls_adj_table"]) {
    QVariantList r;
    r.append(QVariant(row[0].get<double>()));
    // Operator stores at 1/kAdjUiToParamScale; multiply back for UI
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
  EditorHlsModel loaded;
  RecordingSubmitter dummy_sub;
  loaded.setSubmitter(&dummy_sub);
  loaded.loadFromTables(ui_table, range_table, target_hue);

  // The values must be preserved
  EXPECT_NEAR(loaded.lightness(), 45.0, 1.0);
  EXPECT_EQ(loaded.activeHueIndex(), 2)
      << "Active hue swatch should be restored from target_hls";
}

// ── HLS snapshot rebuild integration ───────────────────────────────────────

TEST(EditorLookModelTest, HlsSnapshotRebuildPreservesUiValues) {
  // Simulate what happens when the backend rebuilds the committed snapshot
  // from pipeline state and publishes it to QML. The snapshot patches contain
  // the operator param JSON; BuildSnapshotMap converts to QVariantMap; QML
  // loadFromSnapshot parses it into model tables.

  // Step 1: Create an EditorRenderAdjustmentSnapshot patch as
  // InitializeCommittedSnapshotFromPipeline / UpsertCommittedSnapshot would.
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
  QJsonParseError error;
  const auto      doc = QJsonDocument::fromJson(
      QByteArray::fromStdString(hls_patch_json), &error);
  ASSERT_EQ(error.error, QJsonParseError::NoError);
  ASSERT_TRUE(doc.isObject());
  const auto obj = doc.object();

  auto snapshot = QVariantMap{};
  snapshot.insert(QStringLiteral("hls"), obj.toVariantMap());

  // Step 2: QML loadFromSnapshot -> loadHlsFromSnapshot
  const auto entry = snapshot.value(QStringLiteral("hls")).toMap();
  const auto hls = entry.value(QStringLiteral("HLS")).toMap();

  // Extract tables (matching loadHlsFromSnapshot logic)
  const auto raw_table = hls.value(QStringLiteral("hls_adj_table")).toList();
  const auto ranges = hls.value(QStringLiteral("h_range_table")).toList();

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

  double target_hue = 0.0;
  const auto target = hls.value(QStringLiteral("target_hls")).toList();
  if (target.size() > 0) {
    target_hue = target[0].toDouble();
  }

  // Step 3: Load into a fresh HLS model
  RecordingSubmitter dummy;
  EditorHlsModel loaded;
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
