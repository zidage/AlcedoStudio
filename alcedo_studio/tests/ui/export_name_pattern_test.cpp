//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The file name pattern check of the export panel. The panel (ExportNamingEditor.qml) and the
// export.run command use ExportRecipeBuilder, which reads the pattern with the parser of the
// export (ParseExportFileNamePattern).

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "ui/alcedo_main/album_backend/export_recipe_builder.hpp"

namespace alcedo::ui::test {
namespace {

TEST(ExportNamePatternTest, ValidPatternsAreAccepted) {
  ExportRecipeBuilder builder(nullptr);
  for (const QString& pattern : QStringList{
           QStringLiteral("{source}"), QStringLiteral("{date:yyyy MM dd}-{sequence:0000}"),
           QStringLiteral("{cameraMake}_{cameraModel}_{lens}_{iso}_{aperture}_{shutter}_{focal}"),
           QStringLiteral("{rating}-{{literal}}"), QStringLiteral("Export-{sequence}")}) {
    EXPECT_TRUE(builder.IsFileNamePatternValid(pattern)) << pattern.toStdString();
    EXPECT_FALSE(ExportRecipeBuilder::CheckFileNamePattern(pattern).has_value());
  }
}

TEST(ExportNamePatternTest, InvalidPatternIsRejected) {
  ExportRecipeBuilder builder(nullptr);
  for (const QString& pattern :
       QStringList{QString(), QStringLiteral("{unknown}"), QStringLiteral("{source"),
                   QStringLiteral("source}"), QStringLiteral("{sequence:00x0}"),
                   QStringLiteral("{}"), QStringLiteral("{source:arg}")}) {
    EXPECT_FALSE(builder.IsFileNamePatternValid(pattern)) << pattern.toStdString();
  }
  EXPECT_EQ(ExportRecipeBuilder::CheckFileNamePattern(QStringLiteral("{unknown}")).value_or(""),
            QStringLiteral("The file-name pattern contains an unknown field"));

  ExportRecipeOptions options = ExportRecipeBuilder::Defaults();
  options.file_name_pattern   = QStringLiteral("{source");
  const auto error            = ExportRecipeBuilder::Validate(options);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->field, QStringLiteral("file_name_pattern"));
  EXPECT_EQ(error->message, QStringLiteral("The file-name pattern has an unmatched opening brace"));
}

}  // namespace
}  // namespace alcedo::ui::test
