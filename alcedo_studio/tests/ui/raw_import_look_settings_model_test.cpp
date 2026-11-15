//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The RAW Import section model of Settings (#920): it reads the stored look, edits pending values,
// writes only the rows that the page changed (an app default as a key removal), rejects values
// outside the table, and gives the curve line of the selected Tone Curve preset.

#include "ui/alcedo_main/album_backend/raw_import_look_settings_model.hpp"

#include <gtest/gtest.h>

#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantMap>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "app/raw_import_look_settings.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "ui/alcedo_main/editor_support/modules/curve.hpp"

namespace alcedo::ui::test {
namespace {

constexpr RawImportLookField kAllFields[] = {
    RawImportLookField::ExposureEv,        RawImportLookField::Saturation,
    RawImportLookField::ToneCurvePreset,   RawImportLookField::DrtMethod,
    RawImportLookField::OpenDrtLookPreset, RawImportLookField::LensCorrection,
};

class RawImportLookSettingsModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.isValid());
    settings_file_ = dir_.filePath(QStringLiteral("settings.ini"));
  }

  [[nodiscard]] auto Settings() const -> RawImportLookSettings {
    return RawImportLookSettings(settings_file_);
  }
  [[nodiscard]] auto Contains(RawImportLookField field) const -> bool {
    const QSettings settings(settings_file_, QSettings::IniFormat);
    return settings.contains(RawImportLookSettings::Key(field));
  }
  [[nodiscard]] auto StoredKeyCount() const -> int {
    int count = 0;
    for (const auto field : kAllFields) count += Contains(field) ? 1 : 0;
    return count;
  }

  QTemporaryDir dir_;
  QString       settings_file_;
};

/// The editor curve line of preset @p preset: the same spline as the Tone Curve section.
auto EditorLineOf(const ToneCurvePreset& preset) -> QList<QPointF> {
  std::vector<QPointF> points;
  for (const auto& point : preset.points) points.emplace_back(point.x, point.y);
  return curve::SampleCurveLine(
      curve::SolveCurveSpline(curve::NormalizeCurveControlPoints(points)));
}

TEST_F(RawImportLookSettingsModelTest, WithoutStoredKeysTheRowsShowTheAppDefaults) {
  RawImportLookSettingsModel model(settings_file_);

  EXPECT_DOUBLE_EQ(model.exposure_ev(), 1.5);
  EXPECT_DOUBLE_EQ(model.saturation(), 30.0);
  EXPECT_EQ(model.tone_curve_preset(), QStringLiteral("cl_digital"));
  EXPECT_EQ(model.drt_method(), QStringLiteral("open_drt"));
  EXPECT_EQ(model.open_drt_look_preset(), QStringLiteral("standard"));
  EXPECT_FALSE(model.lens_correction());
  EXPECT_FALSE(model.any_modified());
  for (const auto field : kAllFields) EXPECT_FALSE(model.IsModified(field));
  EXPECT_TRUE(model.error_message().isEmpty());
}

TEST_F(RawImportLookSettingsModelTest, LoadShowsStoredValuesIncludingOnesWrittenByAnotherWriter) {
  RawImportLookSettingsModel model(settings_file_);
  auto                       other_writer = Settings();
  other_writer.SetExposureEv(-2.25);
  other_writer.SetSaturationPercent(-40.0);
  other_writer.SetToneCurvePreset("kodak_2383");
  other_writer.SetDrtMethod(DrtMethod::Aces20);
  other_writer.SetOpenDrtLookPreset("umbra");
  other_writer.SetLensCorrection(true);
  EXPECT_DOUBLE_EQ(model.exposure_ev(), 1.5);

  QSignalSpy pending_changed(&model, &RawImportLookSettingsModel::pendingChanged);
  model.load();

  EXPECT_GE(pending_changed.count(), 1);
  EXPECT_DOUBLE_EQ(model.exposure_ev(), -2.25);
  EXPECT_DOUBLE_EQ(model.saturation(), -40.0);
  EXPECT_EQ(model.tone_curve_preset(), QStringLiteral("kodak_2383"));
  EXPECT_EQ(model.drt_method(), QStringLiteral("aces_2_0"));
  EXPECT_EQ(model.open_drt_look_preset(), QStringLiteral("umbra"));
  EXPECT_TRUE(model.lens_correction());
  for (const auto field : kAllFields) EXPECT_TRUE(model.IsModified(field));
  EXPECT_TRUE(model.any_modified());
}

TEST_F(RawImportLookSettingsModelTest, PendingEditsWriteNothingUntilApply) {
  RawImportLookSettingsModel model(settings_file_);
  EXPECT_TRUE(model.setExposureEv(0.5));
  EXPECT_TRUE(model.setSaturation(10));
  EXPECT_TRUE(model.setToneCurvePreset(QStringLiteral("linear")));
  EXPECT_TRUE(model.setDrtMethod(QStringLiteral("aces_2_0")));
  EXPECT_TRUE(model.setOpenDrtLookPreset(QStringLiteral("sylvan")));
  model.setLensCorrection(true);

  EXPECT_TRUE(model.exposure_ev_modified());
  EXPECT_TRUE(model.saturation_modified());
  EXPECT_TRUE(model.tone_curve_preset_modified());
  EXPECT_TRUE(model.drt_method_modified());
  EXPECT_TRUE(model.open_drt_look_preset_modified());
  EXPECT_TRUE(model.lens_correction_modified());
  EXPECT_EQ(StoredKeyCount(), 0);
  EXPECT_EQ(Settings().Read(), RawImportLook{});
}

TEST_F(RawImportLookSettingsModelTest, ApplyWritesTheChangedValuesThatTheNextImportReads) {
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.setExposureEv(0.734));  // Rounded to the page step of 0.01 EV.
  ASSERT_TRUE(model.setSaturation(-20.4));  // Rounded to the page step of 1.
  ASSERT_TRUE(model.setToneCurvePreset(QStringLiteral("fuji_velvia_50")));
  ASSERT_TRUE(model.setDrtMethod(QStringLiteral("aces_2_0")));
  ASSERT_TRUE(model.setOpenDrtLookPreset(QStringLiteral("aery")));
  model.setLensCorrection(true);
  EXPECT_DOUBLE_EQ(model.exposure_ev(), 0.73);
  EXPECT_DOUBLE_EQ(model.saturation(), -20.0);

  ASSERT_TRUE(model.apply());

  const auto look = Settings().Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, 0.73f);
  EXPECT_FLOAT_EQ(look.saturation, 0.8f);
  EXPECT_EQ(look.tone_curve_preset_id, "fuji_velvia_50");
  EXPECT_EQ(look.drt_method, DrtMethod::Aces20);
  EXPECT_EQ(look.open_drt_look_preset, "aery");
  EXPECT_TRUE(look.lens_correction);
  EXPECT_TRUE(DescribeRawImportLookError(look).empty());
  // The rows show what apply stored.
  EXPECT_DOUBLE_EQ(model.exposure_ev(), 0.73);
  EXPECT_DOUBLE_EQ(model.saturation(), -20.0);
}

TEST_F(RawImportLookSettingsModelTest, ApplyWritesOnlyTheRowsThatThePageChanged) {
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.setSaturation(50));
  // Another writer stores an exposure after the page loaded.
  Settings().SetExposureEv(3.0);

  ASSERT_TRUE(model.apply());

  EXPECT_TRUE(Contains(RawImportLookField::Saturation));
  EXPECT_TRUE(Contains(RawImportLookField::ExposureEv));
  EXPECT_EQ(StoredKeyCount(), 2);
  const auto look = Settings().Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, 3.0f);
  EXPECT_FLOAT_EQ(look.saturation, 1.5f);
}

TEST_F(RawImportLookSettingsModelTest, AValueEqualToTheAppDefaultIsWrittenAsAKeyRemoval) {
  auto stored = Settings();
  stored.SetExposureEv(2.0);
  stored.SetToneCurvePreset("linear");
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.setExposureEv(1.5));
  ASSERT_TRUE(model.setToneCurvePreset(QStringLiteral("cl_digital")));
  EXPECT_FALSE(model.exposure_ev_modified());
  EXPECT_FALSE(model.tone_curve_preset_modified());

  ASSERT_TRUE(model.apply());

  EXPECT_EQ(StoredKeyCount(), 0);
  EXPECT_EQ(Settings().Read(), RawImportLook{});
}

TEST_F(RawImportLookSettingsModelTest, RowResetReturnsThePendingValueAndApplyRemovesTheKey) {
  auto stored = Settings();
  stored.SetSaturationPercent(-60.0);
  stored.SetLensCorrection(true);
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.saturation_modified());

  EXPECT_TRUE(model.resetRow(QStringLiteral("saturation")));
  EXPECT_DOUBLE_EQ(model.saturation(), 30.0);
  EXPECT_FALSE(model.saturation_modified());
  EXPECT_TRUE(model.lens_correction_modified());
  EXPECT_TRUE(Contains(RawImportLookField::Saturation));  // Pending until apply.
  EXPECT_FALSE(model.resetRow(QStringLiteral("contrast")));

  ASSERT_TRUE(model.apply());

  EXPECT_FALSE(Contains(RawImportLookField::Saturation));
  EXPECT_TRUE(Contains(RawImportLookField::LensCorrection));
  EXPECT_TRUE(model.resetRow(QStringLiteral("lensCorrection")));
  ASSERT_TRUE(model.apply());
  EXPECT_EQ(StoredKeyCount(), 0);
  EXPECT_EQ(Settings().Read(), RawImportLook{});
}

TEST_F(RawImportLookSettingsModelTest, ResetAllAndApplyRemoveEveryKey) {
  auto stored = Settings();
  stored.SetExposureEv(-1.0);
  stored.SetSaturationPercent(0.0);
  stored.SetToneCurvePreset("strong_contrast");
  stored.SetDrtMethod(DrtMethod::Aces20);
  stored.SetOpenDrtLookPreset("colorful");
  stored.SetLensCorrection(true);
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.any_modified());

  model.resetAll();
  EXPECT_FALSE(model.any_modified());
  EXPECT_EQ(model.tone_curve_preset(), QStringLiteral("cl_digital"));
  ASSERT_TRUE(model.apply());

  EXPECT_EQ(StoredKeyCount(), 0);
  EXPECT_EQ(Settings().Read(), RawImportLook{});
}

TEST_F(RawImportLookSettingsModelTest, TheOpenDrtLookStaysWhileAces20IsSelected) {
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.setOpenDrtLookPreset(QStringLiteral("dystopic")));
  ASSERT_TRUE(model.setDrtMethod(QStringLiteral("aces_2_0")));
  EXPECT_EQ(model.open_drt_look_preset(), QStringLiteral("dystopic"));
  ASSERT_TRUE(model.apply());

  const auto look = Settings().Read();
  EXPECT_EQ(look.drt_method, DrtMethod::Aces20);
  EXPECT_EQ(look.open_drt_look_preset, "dystopic");
}

TEST_F(RawImportLookSettingsModelTest, CurveLineIsTheEditorLineOfEachPreset) {
  RawImportLookSettingsModel model(settings_file_);
  // The app default preset gives its line without an edit.
  EXPECT_EQ(model.tone_curve_line(), EditorLineOf(*FindToneCurvePreset("cl_digital")));

  std::set<std::vector<std::pair<double, double>>> distinct_lines;
  for (const auto& preset : ToneCurvePresets()) {
    QSignalSpy pending_changed(&model, &RawImportLookSettingsModel::pendingChanged);
    ASSERT_TRUE(model.setToneCurvePreset(
        QString::fromUtf8(preset.id.data(), static_cast<qsizetype>(preset.id.size()))));
    EXPECT_EQ(pending_changed.count(), 1) << preset.id;
    EXPECT_EQ(model.tone_curve_line(), EditorLineOf(preset)) << preset.id;

    const auto control_points = model.tone_curve_points();
    ASSERT_EQ(control_points.size(), static_cast<qsizetype>(preset.points.size())) << preset.id;
    for (qsizetype i = 0; i < control_points.size(); ++i) {
      EXPECT_FLOAT_EQ(static_cast<float>(control_points[i].x()), preset.points[i].x);
      EXPECT_FLOAT_EQ(static_cast<float>(control_points[i].y()), preset.points[i].y);
    }
    std::vector<std::pair<double, double>> line;
    for (const auto& point : model.tone_curve_line()) line.emplace_back(point.x(), point.y());
    distinct_lines.insert(line);
  }
  EXPECT_EQ(distinct_lines.size(), ToneCurvePresets().size());
}

TEST_F(RawImportLookSettingsModelTest, ARejectedValueKeepsThePreviousValueAndNamesTheError) {
  RawImportLookSettingsModel model(settings_file_);
  ASSERT_TRUE(model.setExposureEv(2.0));
  QSignalSpy pending_changed(&model, &RawImportLookSettingsModel::pendingChanged);
  QSignalSpy error_changed(&model, &RawImportLookSettingsModel::errorMessageChanged);

  EXPECT_FALSE(model.setExposureEv(10.5));
  EXPECT_DOUBLE_EQ(model.exposure_ev(), 2.0);
  EXPECT_FALSE(model.error_message().isEmpty());
  EXPECT_EQ(error_changed.count(), 1);
  // The control of the row reads the kept value again.
  EXPECT_EQ(pending_changed.count(), 1);

  EXPECT_FALSE(model.setExposureEv(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(model.setSaturation(-101));
  EXPECT_FALSE(model.setToneCurvePreset(QStringLiteral("film_like")));
  EXPECT_FALSE(model.setDrtMethod(QStringLiteral("film_print")));
  EXPECT_FALSE(model.setOpenDrtLookPreset(QStringLiteral("custom")));
  EXPECT_DOUBLE_EQ(model.saturation(), 30.0);
  EXPECT_EQ(model.tone_curve_preset(), QStringLiteral("cl_digital"));
  EXPECT_EQ(model.drt_method(), QStringLiteral("open_drt"));
  EXPECT_EQ(model.open_drt_look_preset(), QStringLiteral("standard"));

  // An accepted edit clears the error; apply writes the last accepted value only.
  EXPECT_TRUE(model.setSaturation(-100));
  EXPECT_TRUE(model.error_message().isEmpty());
  ASSERT_TRUE(model.apply());
  const auto look = Settings().Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, 2.0f);
  EXPECT_FLOAT_EQ(look.saturation, 0.0f);
  EXPECT_EQ(look.tone_curve_preset_id, "cl_digital");
}

TEST_F(RawImportLookSettingsModelTest, MenusListThePresetsAndMethodsInMenuOrder) {
  const auto presets = RawImportLookSettingsModel::tone_curve_presets();
  ASSERT_EQ(presets.size(), static_cast<qsizetype>(ToneCurvePresets().size()));
  for (qsizetype i = 0; i < presets.size(); ++i) {
    const auto  entry  = presets[i].toMap();
    const auto& preset = ToneCurvePresets()[static_cast<size_t>(i)];
    EXPECT_EQ(entry.value(QStringLiteral("value")).toString().toStdString(), preset.id);
    EXPECT_EQ(entry.value(QStringLiteral("label")).toString().toStdString(), preset.display_name);
    EXPECT_EQ(entry.value(QStringLiteral("group")).toString().toStdString(), preset.group);
  }

  const auto methods = RawImportLookSettingsModel::drt_methods();
  ASSERT_EQ(methods.size(), 2);
  EXPECT_EQ(methods[0].toMap().value(QStringLiteral("value")), QStringLiteral("aces_2_0"));
  EXPECT_EQ(methods[0].toMap().value(QStringLiteral("label")), QStringLiteral("ACES 2.0"));
  EXPECT_EQ(methods[1].toMap().value(QStringLiteral("value")), QStringLiteral("open_drt"));
  EXPECT_EQ(methods[1].toMap().value(QStringLiteral("label")), QStringLiteral("OpenDRT"));

  const auto looks = RawImportLookSettingsModel::open_drt_look_presets();
  ASSERT_EQ(looks.size(), static_cast<qsizetype>(RawImportOpenDrtLookPresets().size()));
  const QStringList expected_labels = {
      QStringLiteral("Standard"), QStringLiteral("Arriba"), QStringLiteral("Sylvan"),
      QStringLiteral("Colorful"), QStringLiteral("Aery"),   QStringLiteral("Dystopic"),
      QStringLiteral("Umbra"),
  };
  for (qsizetype i = 0; i < looks.size(); ++i) {
    EXPECT_EQ(looks[i].toMap().value(QStringLiteral("value")).toString().toStdString(),
              RawImportOpenDrtLookPresets()[static_cast<size_t>(i)]);
    EXPECT_EQ(looks[i].toMap().value(QStringLiteral("label")).toString(), expected_labels[i]);
  }
}

}  // namespace
}  // namespace alcedo::ui::test
