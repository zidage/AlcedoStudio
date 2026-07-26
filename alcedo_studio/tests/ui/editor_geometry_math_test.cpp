//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_geometry_math.hpp"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>

#include "ui/alcedo_main/album_backend/editor_lens_catalog_model.hpp"

namespace alcedo::ui::test {

TEST(EditorGeometryMathTest, AspectPresetCatalogMatchesPipelineOptions) {
  EditorGeometryMath math;

  const auto         presets = math.aspectPresets();
  ASSERT_EQ(presets.size(), 12);
  EXPECT_EQ(presets.front().toMap().value(QStringLiteral("value")).toString(),
            QStringLiteral("free"));
  EXPECT_EQ(presets.at(4).toMap().value(QStringLiteral("value")).toString(),
            QStringLiteral("ratio_16_9"));

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

}  // namespace alcedo::ui::test
