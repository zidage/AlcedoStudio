//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_geometry_math.hpp"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <iterator>

#include "ui/alcedo_main/album_backend/editor_lens_catalog_model.hpp"

namespace alcedo::ui::test {

TEST(EditorGeometryMathTest, AspectMenuUsesReviewedLabelsAndOrderForExistingPresets) {
  constexpr struct {
    const char* id;
    const char* label;
    float       width;
    float       height;
  } expected[] = {
      {"free", "Free", 0.0f, 0.0f},
      {"custom", "Custom", 0.0f, 0.0f},
      {"ratio_1_1", "1:1", 1.0f, 1.0f},
      {"ratio_1_5_1_native_vistavision", "3:2", 1.5f, 1.0f},
      {"ratio_4_3_35mm", "4:3", 4.0f, 3.0f},
      {"ratio_16_9", "16:9", 16.0f, 9.0f},
      {"ratio_1_43_1_70mm_imax", "1.43:1 (IMAX)", 1.43f, 1.0f},
      {"ratio_1_85_1_dci", "1.85:1 (Flat)", 1.85f, 1.0f},
      {"ratio_1_9_1_imax", "1.90:1 (IMAX)", 1.9f, 1.0f},
      {"ratio_2_2_1_70mm", "2.20:1 (70 mm)", 2.2f, 1.0f},
      {"ratio_2_35_1_35mm", "2.35:1 (CinemaScope)", 2.35f, 1.0f},
      {"ratio_2_76_1_panavision_ultra", "2.76:1 (Ultra Panavision 70)", 2.76f, 1.0f},
  };
  EditorGeometryMath math;
  const auto         presets = math.aspectPresets();
  ASSERT_EQ(presets.size(), std::ssize(expected));
  qsizetype index = 0;
  for (const auto& preset : expected) {
    SCOPED_TRACE(preset.id);
    const auto entry = presets.at(index++).toMap();
    EXPECT_EQ(entry.value(QStringLiteral("value")).toString(), QString::fromUtf8(preset.id));
    EXPECT_EQ(entry.value(QStringLiteral("label")).toString(), QString::fromUtf8(preset.label));
    EXPECT_EQ(entry.value(QStringLiteral("width")).toDouble(), preset.width);
    EXPECT_EQ(entry.value(QStringLiteral("height")).toDouble(), preset.height);
  }
}

TEST(EditorGeometryMathTest, PresetLookupResolvesRatioAndLockByIdentifier) {
  EditorGeometryMath math;
  const auto ratio = math.presetRatio(QStringLiteral("ratio_16_9"));
  ASSERT_EQ(ratio.size(), 2);
  EXPECT_DOUBLE_EQ(ratio.at(0).toDouble(), 16.0);
  EXPECT_DOUBLE_EQ(ratio.at(1).toDouble(), 9.0);
  EXPECT_TRUE(math.hasLockedAspect(QStringLiteral("ratio_16_9"), 1.0, 1.0));
  EXPECT_FALSE(math.hasLockedAspect(QStringLiteral("free"), 1.0, 1.0));
}

TEST(EditorGeometryMathTest, ClampAndAspectFitKeepNormalizedBounds) {
  EditorGeometryMath math;

  const auto         clamped = math.clampCropRect(-0.3, 0.9, 1.2, 0.9);
  ASSERT_EQ(clamped.size(), 4);
  EXPECT_DOUBLE_EQ(clamped.at(0).toDouble(), 0.0);
  EXPECT_NEAR(clamped.at(1).toDouble(), 0.1, 1e-6);
  EXPECT_DOUBLE_EQ(clamped.at(2).toDouble(), 1.0);
  EXPECT_NEAR(clamped.at(3).toDouble(), 0.9, 1e-6);

  const auto fitted = math.maxAspectCropRect(2.0, 16.0 / 9.0);
  ASSERT_EQ(fitted.size(), 4);
  EXPECT_NEAR(fitted.at(2).toDouble() / fitted.at(3).toDouble(), 8.0 / 9.0, 1e-5);
  EXPECT_NEAR(fitted.at(0).toDouble() + fitted.at(2).toDouble() * 0.5, 0.5, 1e-5);
  EXPECT_NEAR(fitted.at(1).toDouble() + fitted.at(3).toDouble() * 0.5, 0.5, 1e-5);

  const auto resized = math.resizeAspectCropRect(0.1, 0.2, 0.6, 0.4, 2.0, 1.5, true);
  ASSERT_EQ(resized.size(), 4);
  EXPECT_NEAR(resized.at(2).toDouble() / resized.at(3).toDouble(), 0.75, 1e-5);
  EXPECT_NEAR(resized.at(0).toDouble() + resized.at(2).toDouble() * 0.5, 0.4, 1e-5);
  EXPECT_NEAR(resized.at(1).toDouble() + resized.at(3).toDouble() * 0.5, 0.4, 1e-5);
}

TEST(EditorGeometryMathTest, LensCatalogProvidesDefaultOperatorShape) {
  EditorLensCatalogModel catalog;

  const auto             defaults = QJsonDocument::fromJson(catalog.defaultParamsJson().toUtf8());
  ASSERT_TRUE(defaults.isObject());
  ASSERT_TRUE(defaults.object().contains(QStringLiteral("lens_calib")));
  EXPECT_TRUE(catalog.modelsForBrand(QStringLiteral("missing-brand")).isEmpty());
}

TEST(EditorGeometryMathTest, LensCatalogListsProfileNamesOfShippedLensfunDatabase) {
  EditorLensCatalogModel catalog;
  const auto             has_model = [&catalog](const char* brand, const char* model) {
    for (const auto& entry : catalog.modelsForBrand(QString::fromUtf8(brand))) {
      if (entry.toMap().value(QStringLiteral("value")).toString() == QString::fromUtf8(model)) {
        return true;
      }
    }
    return false;
  };

  // Profiles added in Lensfun 43f9e001.
  EXPECT_TRUE(has_model("Nikon", "Nikkor Z 24-105mm f/4-7.1"));
  EXPECT_TRUE(has_model("Nikon", "Nikkor Z DX 16-50mm f/2.8 VR"));
  EXPECT_TRUE(has_model("Sony", "FE 50mm f/1.4 GM"));
  EXPECT_TRUE(has_model("Canon", "Canon RF 50mm F1.4 L VCM"));
  // Profiles renamed in Lensfun 43f9e001: the catalog lists only the new names.
  EXPECT_TRUE(has_model("Tamron", "Tamron 17-70mm F2.8 B070 Z/RF/X/E"));
  EXPECT_FALSE(has_model("Tamron", "E 17-70mm F2.8 B070"));
  EXPECT_TRUE(has_model("Sony", "E PZ 16-50mm f/3.5-5.6 OSS I/II"));
  EXPECT_FALSE(has_model("Sony", "E PZ 16-50mm f/3.5-5.6 OSS"));
}

}  // namespace alcedo::ui::test
