//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ExportRecipeBuilder rules that ExportInspectorPanel.qml had: the defaults, the limits of the
// size values, the bit depths of each format, and the output folder rule.

#include "ui/alcedo_main/album_backend/export_recipe_builder.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QList>
#include <QLocale>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariantMap>

namespace alcedo::ui::test {
namespace {

using Builder = ExportRecipeBuilder;

TEST(ExportRecipeBuilderTest, DefaultsMatchPreviousPanelDefaults) {
  const ExportRecipeOptions defaults = Builder::Defaults();
  EXPECT_EQ(defaults.format, QStringLiteral("JPEG"));
  EXPECT_EQ(defaults.bit_depth, 8);
  EXPECT_EQ(defaults.quality, 95);
  EXPECT_EQ(defaults.png_level, 5);
  EXPECT_EQ(defaults.tiff_compression, QStringLiteral("NONE"));
  EXPECT_TRUE(defaults.include_metadata);
  EXPECT_TRUE(defaults.embed_icc_profile);
  EXPECT_TRUE(defaults.include_gps);
  EXPECT_EQ(defaults.ultra_hdr_quality, 95);
  EXPECT_TRUE(defaults.ultra_hdr_dither);
  EXPECT_EQ(defaults.ultra_hdr_long_edge, 8192);
  EXPECT_FALSE(defaults.subfolder_enabled);
  EXPECT_EQ(defaults.subfolder, QStringLiteral("Processed"));
  EXPECT_EQ(defaults.resize_mode, QStringLiteral("original"));
  EXPECT_EQ(defaults.long_edge, 2048);
  EXPECT_EQ(defaults.width, 2048);
  EXPECT_EQ(defaults.height, 2048);
  EXPECT_EQ(defaults.print_width, 210);
  EXPECT_EQ(defaults.print_height, 297);
  EXPECT_EQ(defaults.print_unit, QStringLiteral("mm"));
  EXPECT_EQ(defaults.dpi, 300);
  EXPECT_EQ(defaults.file_name_pattern, QStringLiteral("{source}"));
  EXPECT_FALSE(Builder::Validate(defaults).has_value());

  // The map form that the panel reads has the same values.
  Builder           builder(nullptr);
  const QVariantMap map = builder.DefaultOptions();
  EXPECT_EQ(map.value(QStringLiteral("format")).toString(), QStringLiteral("JPEG"));
  EXPECT_EQ(map.value(QStringLiteral("long_edge")).toInt(), 2048);
  EXPECT_EQ(map.value(QStringLiteral("dpi")).toInt(), 300);
}

TEST(ExportRecipeBuilderTest, BitDepthNotAllowedForFormatIsRejected) {
  EXPECT_EQ(Builder::AllowedBitDepths(QStringLiteral("JPEG")), (QList<int>{8}));
  EXPECT_EQ(Builder::AllowedBitDepths(QStringLiteral("PNG")), (QList<int>{8, 16}));
  EXPECT_EQ(Builder::AllowedBitDepths(QStringLiteral("TIFF")), (QList<int>{8, 16, 32}));
  EXPECT_EQ(Builder::AllowedBitDepths(QStringLiteral("EXR")), (QList<int>{16, 32}));
  EXPECT_EQ(Builder::PreferredBitDepth(QStringLiteral("JPEG")), 8);
  EXPECT_EQ(Builder::PreferredBitDepth(QStringLiteral("EXR")), 16);

  ExportRecipeOptions options = Builder::Defaults();
  options.bit_depth           = 16;
  const auto error            = Builder::Validate(options);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->field, QStringLiteral("bit_depth"));
  EXPECT_EQ(error->message, QStringLiteral("bit_depth for JPEG must be one of: 8"));

  options.format    = QStringLiteral("EXR");
  options.bit_depth = 8;
  EXPECT_EQ(Builder::Validate(options)->field, QStringLiteral("bit_depth"));
  options.bit_depth = 32;
  EXPECT_FALSE(Builder::Validate(options).has_value());

  options.format          = QStringLiteral("WEBP");
  const auto format_error = Builder::Validate(options);
  ASSERT_TRUE(format_error.has_value());
  EXPECT_EQ(format_error->message, QStringLiteral("format must be one of: JPEG, PNG, TIFF, EXR"));
}

TEST(ExportRecipeBuilderTest, SizeValuesOutsideLimitsAreRejectedWithTheRange) {
  ExportRecipeOptions options = Builder::Defaults();
  options.resize_mode         = QStringLiteral("long_edge");
  options.long_edge           = 100;
  const auto long_edge_error  = Builder::Validate(options);
  ASSERT_TRUE(long_edge_error.has_value());
  EXPECT_EQ(long_edge_error->field, QStringLiteral("long_edge"));
  EXPECT_EQ(long_edge_error->message, QStringLiteral("long_edge must be in [256, 16384]"));
  options.long_edge = 256;
  EXPECT_FALSE(Builder::Validate(options).has_value());
  options.long_edge = 16384.5;
  EXPECT_TRUE(Builder::Validate(options).has_value());

  // Only the values of the selected mode are checked.
  options.long_edge   = 100;
  options.resize_mode = QStringLiteral("bounds");
  options.width       = 100000;
  options.height      = 0;
  EXPECT_EQ(Builder::Validate(options)->field, QStringLiteral("height"));
  options.height = 1;
  EXPECT_FALSE(Builder::Validate(options).has_value());

  options.resize_mode = QStringLiteral("print");
  options.print_width = 0;
  EXPECT_EQ(Builder::Validate(options)->field, QStringLiteral("print_width"));
  options.print_width = 100000;
  options.dpi         = 10001;
  EXPECT_EQ(Builder::Validate(options)->message, QStringLiteral("dpi must be in [1, 10000]"));
  options.dpi = 1;
  EXPECT_FALSE(Builder::Validate(options).has_value());

  options.quality = 0;
  EXPECT_EQ(Builder::Validate(options)->field, QStringLiteral("quality"));
  options.quality   = 100;
  options.png_level = 10;
  EXPECT_EQ(Builder::Validate(options)->message, QStringLiteral("png_level must be in [0, 9]"));
}

TEST(ExportRecipeBuilderTest, PanelTextValuesAreReadLikeThePanelFields) {
  Builder builder(nullptr);
  // Trimmed integer text is valid; text that is not a number or not an integer is not.
  EXPECT_TRUE(
      builder.ValidateOptions(QVariantMap{{"resize_mode", "long_edge"}, {"long_edge", " 512 "}})
          .value(QStringLiteral("valid"))
          .toBool());
  EXPECT_FALSE(
      builder.ValidateOptions(QVariantMap{{"resize_mode", "long_edge"}, {"long_edge", "5x"}})
          .value(QStringLiteral("valid"))
          .toBool());
  EXPECT_FALSE(builder.ValidateOptions(QVariantMap{{"resize_mode", "bounds"}, {"width", "12.5"}})
                   .value(QStringLiteral("valid"))
                   .toBool());
  // Print sizes are read in the default locale, as Number.fromLocaleString did.
  const QString print_width = QLocale().toString(210.5);
  EXPECT_TRUE(builder
                  .ValidateOptions(QVariantMap{
                      {"resize_mode", "print"}, {"print_width", print_width}, {"dpi", "300"}})
                  .value(QStringLiteral("valid"))
                  .toBool());
  const QVariantMap rejected =
      builder.ValidateOptions(QVariantMap{{"resize_mode", "print"}, {"print_height", ""}});
  EXPECT_FALSE(rejected.value(QStringLiteral("valid")).toBool());
  EXPECT_EQ(rejected.value(QStringLiteral("field")).toString(), QStringLiteral("print_height"));
}

TEST(ExportRecipeBuilderTest, OutputFolderAppendsEnabledSubfolder) {
  ExportRecipeOptions options = Builder::Defaults();
  options.out_dir             = QStringLiteral("D:/Exports/");
  EXPECT_EQ(Builder::OutputFolder(options), QStringLiteral("D:/Exports/"));
  options.subfolder_enabled = true;
  options.subfolder         = QStringLiteral("  Web  ");
  EXPECT_EQ(Builder::OutputFolder(options), QStringLiteral("D:/Exports/Web"));
  options.out_dir = QStringLiteral("D:\\Exports\\\\");
  EXPECT_EQ(Builder::OutputFolder(options), QStringLiteral("D:\\Exports/Web"));
  options.subfolder = QStringLiteral("   ");
  EXPECT_EQ(Builder::OutputFolder(options), QStringLiteral("D:\\Exports\\\\"));
}

TEST(ExportRecipeBuilderTest, StartWithRejectedOptionDoesNotReachTheHandler) {
  Builder             builder(nullptr);
  ExportRecipeOptions options = Builder::Defaults();
  options.resize_mode         = QStringLiteral("long_edge");
  options.long_edge           = 100;
  const auto error            = builder.Start(options, {});
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->field, QStringLiteral("long_edge"));
}

TEST(ExportRecipeBuilderTest, UltraHdrLongEdgeOutsideUltraHdrLimitIsRejected) {
  ExportRecipeOptions options = Builder::Defaults();
  options.ultra_hdr_long_edge = 8193;
  const auto error            = Builder::Validate(options);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->message, QStringLiteral("ultra_hdr_long_edge must be in [256, 8192]"));
  options.ultra_hdr_long_edge = 256;
  EXPECT_FALSE(Builder::Validate(options).has_value());

  Builder builder(nullptr);
  EXPECT_FALSE(builder.ValidateOptions(QVariantMap{{"ultra_hdr_long_edge", "4096.5"}})
                   .value(QStringLiteral("valid"))
                   .toBool());
  const QVariantMap map = Builder::ToVariantMap(Builder::FromVariantMap(
      QVariantMap{{"include_gps", false}, {"ultra_hdr_long_edge", " 4096 "}}));
  EXPECT_FALSE(map.value(QStringLiteral("include_gps")).toBool());
  EXPECT_EQ(map.value(QStringLiteral("ultra_hdr_long_edge")).toInt(), 4096);
}

TEST(ExportRecipeBuilderTest, BuiltInPresetsAreValidAndStandardMatchesTheDefaults) {
  const auto presets = Builder::BuiltInPresets();
  ASSERT_EQ(presets.size(), 7);
  QSet<QString> ids;
  for (const auto& preset : presets) {
    SCOPED_TRACE(preset.id.toStdString());
    EXPECT_TRUE(preset.built_in);
    EXPECT_TRUE(preset.id.startsWith(QStringLiteral("builtin:")));
    EXPECT_FALSE(Builder::Validate(preset.options).has_value());
    EXPECT_LE(preset.options.ultra_hdr_long_edge, Builder::kUltraHdrMaxLongEdge);
    ids.insert(preset.id);
  }
  EXPECT_EQ(ids.size(), presets.size());
  EXPECT_EQ(Builder::MatchingPresetId(Builder::Defaults()), QStringLiteral("builtin:standard"));

  // Each built-in preset matches itself and no earlier preset.
  for (const auto& preset : presets) {
    EXPECT_EQ(Builder::MatchingPresetId(preset.options), preset.id);
  }

  const auto web = presets[1];
  EXPECT_EQ(web.id, QStringLiteral("builtin:web"));
  EXPECT_EQ(web.options.format, QStringLiteral("JPEG"));
  EXPECT_EQ(web.options.resize_mode, QStringLiteral("long_edge"));
  EXPECT_EQ(web.options.long_edge, 2048);
  EXPECT_FALSE(web.options.include_gps);
  EXPECT_EQ(web.options.ultra_hdr_long_edge, 2048);

  const auto exr = presets.back();
  EXPECT_EQ(exr.id, QStringLiteral("builtin:exr_archive"));
  EXPECT_EQ(exr.options.format, QStringLiteral("EXR"));
  EXPECT_EQ(exr.options.ultra_hdr_long_edge, Builder::kUltraHdrMaxLongEdge);
}

TEST(ExportRecipeBuilderTest, PresetMatchComparesOnlyTheSelectedFormatAndResizeValues) {
  const ExportRecipeOptions web = Builder::BuiltInPresets()[1].options;

  ExportRecipeOptions changed = web;
  changed.png_level           = 9;  // Not a JPEG value.
  changed.print_width         = 99;  // Not a long edge value.
  changed.out_dir             = QStringLiteral("D:/Elsewhere");
  EXPECT_EQ(Builder::MatchingPresetId(changed), QStringLiteral("builtin:web"));

  changed.quality = 81;
  EXPECT_TRUE(Builder::MatchingPresetId(changed).isEmpty());
  changed.quality     = web.quality;
  changed.include_gps = true;
  EXPECT_TRUE(Builder::MatchingPresetId(changed).isEmpty());
  changed.include_gps         = false;
  changed.ultra_hdr_long_edge = 4096;
  EXPECT_TRUE(Builder::MatchingPresetId(changed).isEmpty());
}

TEST(ExportRecipeBuilderTest, ApplyPresetKeepsTheDestination) {
  ExportRecipeOptions current = Builder::Defaults();
  current.out_dir             = QStringLiteral("D:/Exports");
  current.subfolder_enabled   = true;
  current.subfolder           = QStringLiteral("Client");
  current.quality             = 42;

  Builder           builder(nullptr);
  const QVariantMap applied =
      builder.OptionsWithPreset(Builder::ToVariantMap(current), QStringLiteral("builtin:tiff16"));
  EXPECT_EQ(applied.value(QStringLiteral("format")).toString(), QStringLiteral("TIFF"));
  EXPECT_EQ(applied.value(QStringLiteral("bit_depth")).toInt(), 16);
  EXPECT_EQ(applied.value(QStringLiteral("quality")).toInt(), 95);
  EXPECT_EQ(applied.value(QStringLiteral("out_dir")).toString(), QStringLiteral("D:/Exports"));
  EXPECT_TRUE(applied.value(QStringLiteral("subfolder_enabled")).toBool());
  EXPECT_EQ(applied.value(QStringLiteral("subfolder")).toString(), QStringLiteral("Client"));

  // An unknown preset leaves the options unchanged.
  EXPECT_EQ(builder.OptionsWithPreset(Builder::ToVariantMap(current), QStringLiteral("user:None")),
            Builder::ToVariantMap(current));
}

/// QSettings in an INI file of a temporary folder, so the tests do not touch the user settings.
class ExportRecipePresetSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ExportRecipeBuilderTest"));
    QSettings{}.clear();
  }

  void TearDown() override { QSettings{}.clear(); }

  QTemporaryDir settings_dir_;
};

TEST_F(ExportRecipePresetSettingsTest, UserPresetIsSavedMatchedReplacedAndDeleted) {
  Builder    builder(nullptr);
  QSignalSpy presets_changed(&builder, &Builder::PresetsChanged);

  ExportRecipeOptions client = Builder::Defaults();
  client.quality             = 88;
  client.file_name_pattern   = QStringLiteral("Client-{sequence:000}");
  client.out_dir             = QStringLiteral("D:/NotStored");
  const auto saved           = builder.SaveUserPreset(QStringLiteral("  Client  "), client, {});
  ASSERT_TRUE(std::holds_alternative<QString>(saved));
  EXPECT_EQ(std::get<QString>(saved), QStringLiteral("user:Client"));
  EXPECT_EQ(presets_changed.count(), 1);

  const auto user_presets = Builder::UserPresets();
  ASSERT_EQ(user_presets.size(), 1);
  EXPECT_EQ(user_presets[0].name, QStringLiteral("Client"));
  EXPECT_FALSE(user_presets[0].built_in);
  EXPECT_TRUE(user_presets[0].options.out_dir.isEmpty());
  EXPECT_EQ(Builder::MatchingPresetId(client), QStringLiteral("user:Client"));
  const QVariantList listed = builder.PresetList();
  ASSERT_EQ(listed.size(), 8);
  EXPECT_EQ(listed.back().toMap().value(QStringLiteral("id")).toString(),
            QStringLiteral("user:Client"));
  EXPECT_FALSE(listed.back().toMap().value(QStringLiteral("builtIn")).toBool());

  // Saving under a new name with the old id renames the preset.
  client.quality       = 70;
  const auto renamed   = builder.SaveUserPreset(QStringLiteral("Proofs"), client,
                                                QStringLiteral("user:Client"));
  ASSERT_TRUE(std::holds_alternative<QString>(renamed));
  ASSERT_EQ(Builder::UserPresets().size(), 1);
  EXPECT_EQ(Builder::UserPresets()[0].name, QStringLiteral("Proofs"));
  EXPECT_EQ(Builder::UserPresets()[0].options.quality, 70);

  // Invalid names and options are rejected; built-in presets cannot be deleted.
  const auto empty_name = builder.SaveUserPreset(QStringLiteral("   "), client, {});
  ASSERT_TRUE(std::holds_alternative<ExportRecipeError>(empty_name));
  EXPECT_EQ(std::get<ExportRecipeError>(empty_name).field, QStringLiteral("name"));
  ExportRecipeOptions invalid = client;
  invalid.quality             = 0;
  EXPECT_TRUE(std::holds_alternative<ExportRecipeError>(
      builder.SaveUserPreset(QStringLiteral("Invalid"), invalid, {})));
  EXPECT_FALSE(builder.DeletePreset(QStringLiteral("builtin:web")));

  EXPECT_TRUE(builder.DeletePreset(QStringLiteral("user:proofs")));
  EXPECT_TRUE(Builder::UserPresets().isEmpty());
  EXPECT_FALSE(builder.DeletePreset(QStringLiteral("user:Proofs")));
}

TEST_F(ExportRecipePresetSettingsTest, LastUsedOptionsAreRestoredWithoutTheDestination) {
  EXPECT_TRUE(Builder::HasSamePresetOptions(Builder::LastUsedOptions(), Builder::Defaults()));

  ExportRecipeOptions options = Builder::BuiltInPresets()[2].options;
  options.out_dir             = QStringLiteral("D:/Exports");
  options.subfolder_enabled   = true;
  Builder::SaveLastUsedOptions(options);
  const ExportRecipeOptions restored = Builder::LastUsedOptions();
  EXPECT_TRUE(Builder::HasSamePresetOptions(restored, options));
  EXPECT_TRUE(restored.out_dir.isEmpty());
  EXPECT_FALSE(restored.subfolder_enabled);
  EXPECT_EQ(Builder::MatchingPresetId(restored), QStringLiteral("builtin:social"));

  // Invalid options are not stored, and an invalid stored entry gives the defaults.
  ExportRecipeOptions invalid = options;
  invalid.quality             = 0;
  Builder::SaveLastUsedOptions(invalid);
  EXPECT_EQ(Builder::LastUsedOptions().quality, options.quality);
  QSettings{}.setValue(QStringLiteral("export/lastOptions"),
                       QVariantMap{{QStringLiteral("format"), QStringLiteral("WEBP")}});
  EXPECT_TRUE(Builder::HasSamePresetOptions(Builder::LastUsedOptions(), Builder::Defaults()));
}

}  // namespace
}  // namespace alcedo::ui::test
