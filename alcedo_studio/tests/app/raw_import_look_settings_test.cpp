//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The RAW import look in QSettings (#918): each key reads back what its setter wrote, a missing
// or unusable stored value reads as the product Default, and the resets remove keys.

#include "app/raw_import_look_settings.hpp"

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>
#include <QVariant>
#include <chrono>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

class RawImportLookSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    temp_ = fs::temp_directory_path() /
            ("alcedo_raw_import_look_settings_test_" + std::to_string(device()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp_);
    settings_file_ = QString::fromStdWString((temp_ / "settings.ini").wstring());
  }
  void TearDown() override {
    std::error_code error;
    fs::remove_all(temp_, error);
  }

  /// Write @p value under @p field directly, as a settings file of another version could hold it.
  void StoreRaw(RawImportLookField field, const QVariant& value) const {
    QSettings settings(settings_file_, QSettings::IniFormat);
    settings.setValue(RawImportLookSettings::Key(field), value);
  }
  [[nodiscard]] auto Contains(RawImportLookField field) const -> bool {
    const QSettings settings(settings_file_, QSettings::IniFormat);
    return settings.contains(RawImportLookSettings::Key(field));
  }

  fs::path temp_;
  QString  settings_file_;
};

TEST_F(RawImportLookSettingsTest, EmptySettingsReadTheProductDefault) {
  const RawImportLookSettings settings(settings_file_);
  EXPECT_EQ(settings.Read(), RawImportLook{});
  // The product Default document, so the root id of an import with untouched settings stays.
  EXPECT_EQ(CreateDefaultPipelineDocument(settings.Read()).ToJson(),
            CreateDefaultPipelineDocument().ToJson());
}

TEST_F(RawImportLookSettingsTest, EachSetterWritesOneValueThatReadReturns) {
  RawImportLookSettings settings(settings_file_);
  settings.SetExposureEv(0.5);
  settings.SetSaturationPercent(10.0);
  settings.SetToneCurvePreset("kodak_2383");
  settings.SetDrtMethod(DrtMethod::Aces20);
  settings.SetOpenDrtLookPreset("sylvan");
  settings.SetLensCorrection(true);

  // A second object reads the same file.
  const auto look = RawImportLookSettings(settings_file_).Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, 0.5f);
  EXPECT_FLOAT_EQ(look.saturation, 1.1f);
  EXPECT_EQ(look.tone_curve_preset_id, "kodak_2383");
  EXPECT_EQ(look.drt_method, DrtMethod::Aces20);
  EXPECT_EQ(look.open_drt_look_preset, "sylvan");
  EXPECT_TRUE(look.lens_correction);
  EXPECT_TRUE(DescribeRawImportLookError(look).empty());
}

TEST_F(RawImportLookSettingsTest, StoredProductDefaultValuesReadAsTheExactProductDefault) {
  RawImportLookSettings settings(settings_file_);
  settings.SetExposureEv(1.5);
  settings.SetSaturationPercent(30.0);
  settings.SetToneCurvePreset("cl_digital");
  settings.SetDrtMethod(DrtMethod::OpenDrt);
  settings.SetOpenDrtLookPreset("standard");
  settings.SetLensCorrection(false);
  // Exact equality: editor value +30 gives the float of kDefaultPipelineSaturation.
  EXPECT_EQ(settings.Read(), RawImportLook{});
}

TEST_F(RawImportLookSettingsTest, SaturationPercentMapsTheEditorRangeToTheMultiplier) {
  RawImportLookSettings settings(settings_file_);
  settings.SetSaturationPercent(-100.0);
  EXPECT_FLOAT_EQ(settings.Read().saturation, 0.0f);
  settings.SetSaturationPercent(100.0);
  EXPECT_FLOAT_EQ(settings.Read().saturation, 2.0f);
  settings.SetSaturationPercent(0.0);
  EXPECT_FLOAT_EQ(settings.Read().saturation, 1.0f);
}

TEST_F(RawImportLookSettingsTest, SettersRejectValuesOutsideTheTableAndKeepTheStoredValue) {
  RawImportLookSettings settings(settings_file_);
  settings.SetExposureEv(2.0);
  EXPECT_THROW(settings.SetExposureEv(10.5), std::invalid_argument);
  EXPECT_THROW(settings.SetExposureEv(std::numeric_limits<double>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_THROW(settings.SetSaturationPercent(-101.0), std::invalid_argument);
  EXPECT_THROW(settings.SetToneCurvePreset("filmic"), std::invalid_argument);
  EXPECT_THROW(settings.SetDrtMethod(DrtMethod::FilmPrint), std::invalid_argument);
  EXPECT_THROW(settings.SetOpenDrtLookPreset("custom"), std::invalid_argument);
  EXPECT_FLOAT_EQ(settings.Read().exposure_ev, 2.0f);
  EXPECT_FALSE(Contains(RawImportLookField::Saturation));
  EXPECT_FALSE(Contains(RawImportLookField::ToneCurvePreset));
}

TEST_F(RawImportLookSettingsTest, UnusableStoredValuesReadAsTheProductDefault) {
  StoreRaw(RawImportLookField::ExposureEv, QStringLiteral("bright"));
  StoreRaw(RawImportLookField::Saturation, 250.0);
  StoreRaw(RawImportLookField::ToneCurvePreset, QStringLiteral("filmic"));
  StoreRaw(RawImportLookField::DrtMethod, QStringLiteral("film_print"));
  StoreRaw(RawImportLookField::OpenDrtLookPreset, QStringLiteral("custom"));
  StoreRaw(RawImportLookField::LensCorrection, QStringLiteral("yes"));
  EXPECT_EQ(RawImportLookSettings(settings_file_).Read(), RawImportLook{});

  StoreRaw(RawImportLookField::ExposureEv, 25.0);
  EXPECT_FLOAT_EQ(RawImportLookSettings(settings_file_).Read().exposure_ev,
                  kDefaultPipelineExposureEv);
}

TEST_F(RawImportLookSettingsTest, ResetRemovesOneKeyAndResetAllRemovesEveryKey) {
  RawImportLookSettings settings(settings_file_);
  settings.SetExposureEv(-1.0);
  settings.SetSaturationPercent(-20.0);
  settings.SetLensCorrection(true);

  settings.Reset(RawImportLookField::ExposureEv);
  EXPECT_FALSE(Contains(RawImportLookField::ExposureEv));
  auto look = settings.Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, kDefaultPipelineExposureEv);
  EXPECT_FLOAT_EQ(look.saturation, 0.8f);
  EXPECT_TRUE(look.lens_correction);

  settings.ResetAll();
  for (const auto field :
       {RawImportLookField::ExposureEv, RawImportLookField::Saturation,
        RawImportLookField::ToneCurvePreset, RawImportLookField::DrtMethod,
        RawImportLookField::OpenDrtLookPreset, RawImportLookField::LensCorrection}) {
    EXPECT_FALSE(Contains(field));
  }
  EXPECT_EQ(settings.Read(), RawImportLook{});
}

}  // namespace
}  // namespace alcedo
