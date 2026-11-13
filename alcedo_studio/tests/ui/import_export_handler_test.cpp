//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The import name filters that the QML file dialog reads come from the import categories.

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "type/supported_file_type.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"

namespace alcedo::ui::test {
namespace {

auto Pattern(std::string_view extension) -> QString {
  return QStringLiteral("*") +
         QString::fromLatin1(extension.data(), static_cast<qsizetype>(extension.size()));
}

TEST(ImportExportHandlerTest, NameFiltersCoverEveryRawExtension) {
  const QStringList filters = ImportExportHandler::SupportedImportNameFilters();
  for (const auto extension : kRawExtensions) {
    EXPECT_TRUE(filters.contains(Pattern(extension))) << extension;
  }
}

TEST(ImportExportHandlerTest, NameFiltersCoverEveryRasterExtensionAndNothingElse) {
  const QStringList filters = ImportExportHandler::SupportedImportNameFilters();
  for (const auto& raster : kRasterImportExtensions) {
    EXPECT_TRUE(filters.contains(Pattern(raster.extension))) << raster.extension;
  }
  EXPECT_EQ(filters.size(),
            static_cast<qsizetype>(kRawExtensions.size() + kRasterImportExtensions.size()));
  for (const QString& filter : filters) {
    const std::string extension = filter.mid(1).toStdString();
    EXPECT_NE(CategoryForExtension(extension), ImportFileCategory::Other) << extension;
  }
  EXPECT_EQ(ImportExportHandler::SupportedImportPatterns(), filters.join(QLatin1Char(' ')));
}

}  // namespace
}  // namespace alcedo::ui::test
