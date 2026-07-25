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
  EXPECT_TRUE(ct.contains(QStringLiteral("cct")));
  EXPECT_TRUE(ct.contains(QStringLiteral("tint")));
  EXPECT_TRUE(ct.contains(QStringLiteral("resolved_cct")));
  EXPECT_TRUE(ct.contains(QStringLiteral("resolved_tint")));
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
  EXPECT_NEAR(ct.value(QStringLiteral("cct")).toDouble(), 6400.0, 1e-3);
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
  const auto root = ParseObject(sub.lastSettledParams());
  EXPECT_EQ(root.value(QStringLiteral("ocio_lmt")).toString(), QString());
}

}  // namespace alcedo::ui::test
