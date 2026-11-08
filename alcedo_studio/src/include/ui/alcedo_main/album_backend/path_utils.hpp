//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QDate>
#include <QImage>
#include <QString>
#include <QVariantList>

#include <array>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

#include "app/export_service.hpp"

namespace alcedo::ui::album_util {

auto WStringToQString(const std::wstring& value) -> QString;
auto PathToQString(const std::filesystem::path& path) -> QString;
auto RootFsPath() -> std::filesystem::path;
auto RootPathText() -> QString;
auto InputToPath(const QString& raw) -> std::optional<std::filesystem::path>;
auto FolderPathToDisplay(const std::filesystem::path& path) -> QString;
auto QStringToFsPath(const QString& text) -> std::filesystem::path;

auto DateFromTimeT(std::time_t value) -> QDate;
auto DateFromExifString(const std::string& value) -> QDate;

auto ExtensionUpper(const std::filesystem::path& path) -> QString;
auto ExtensionFromFileName(const QString& name) -> QString;

auto DataUrlFromImage(const QImage& image) -> QString;
auto MatRgba32fToQImageCopy(const cv::Mat& rgba32fOrU8) -> QImage;

auto ExtensionForExportFormat(ImageFormatType format) -> std::string;
auto FormatFromName(const QString& value) -> ImageFormatType;
auto HdrExportModeFromName(const QString& value) -> ExportFormatOptions::HDR_EXPORT_MODE;
auto BitDepthFromInt(int value) -> ExportFormatOptions::BIT_DEPTH;
auto TiffCompressFromName(const QString& value) -> ExportFormatOptions::TIFF_COMPRESS;
auto ExportPathForOptions(const std::filesystem::path& srcPath,
                          const std::filesystem::path& outDir,
                          sl_element_id_t elementId, image_id_t imageId,
                          ImageFormatType format) -> std::filesystem::path;

auto ListCubeLutsInDir(const std::filesystem::path& dir) -> std::vector<std::filesystem::path>;
auto NearlyEqual(float a, float b) -> bool;
auto ClampToRange(double value, double minValue, double maxValue) -> float;

constexpr std::array<const char*, 6> kThumbnailAccentPalette = {
    "#5AA2FF", "#4CC9A6", "#F7B267", "#E08BFF", "#7AD1FF", "#9BD65B",
};
auto AccentForIndex(size_t index) -> QString;
auto ExportTargetKey(sl_element_id_t elementId, image_id_t imageId) -> uint64_t;

auto EscapeSqlStringLiteral(const std::string& text) -> std::string;
auto EscapeSqlIdentifier(const std::string& text) -> std::string;

auto ValidateProjectName(const QString& rawName, QString* errorOut) -> std::optional<QString>;
/// Removes @p dir and everything in it. On a failure, logs the OS error, then removes the files
/// that are left one by one and logs each file that still cannot be removed, with its OS error
/// and (on Windows) the processes that hold it. Returns true when @p dir no longer exists.
auto CleanupWorkspaceDirectory(const std::filesystem::path& dir) -> bool;
auto EnsureDirectoryExists(const std::filesystem::path& dir) -> bool;

}  // namespace alcedo::ui::album_util
