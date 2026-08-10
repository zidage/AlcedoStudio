//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/path_utils.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include <opencv2/opencv.hpp>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::album_util {

auto WStringToQString(const std::wstring& value) -> QString {
  return QString::fromStdWString(value);
}

auto PathToQString(const std::filesystem::path& path) -> QString {
#if defined(_WIN32)
  return QString::fromStdWString(path.wstring());
#else
  return QString::fromUtf8(path.string().c_str());
#endif
}

auto RootFsPath() -> std::filesystem::path {
  return std::filesystem::path(L"/");
}

auto RootPathText() -> QString {
  return "\\";
}

auto InputToPath(const QString& raw) -> std::optional<std::filesystem::path> {
  const QString trimmed = raw.trimmed();
  if (trimmed.isEmpty()) {
    return std::nullopt;
  }

  const QUrl maybe_url(trimmed);
  if (maybe_url.isValid() && maybe_url.scheme() == QStringLiteral("file")) {
    const QString local = maybe_url.toLocalFile();
    if (local.isEmpty()) {
      return std::nullopt;
    }
#if defined(_WIN32)
    return std::filesystem::path(local.toStdWString());
#else
    return std::filesystem::path(local.toStdString());
#endif
  }

#if defined(_WIN32)
  return std::filesystem::path(trimmed.toStdWString());
#else
  return std::filesystem::path(trimmed.toStdString());
#endif
}

auto FolderPathToDisplay(const std::filesystem::path& path) -> QString {
  if (path.empty()) {
    return RootPathText();
  }
#if defined(_WIN32)
  const QString text = QString::fromStdWString(path.generic_wstring());
#else
  const QString text = QString::fromStdString(path.generic_string());
#endif
  if (text == "/" || text == "\\") {
    return RootPathText();
  }
  QString normalized = text;
  normalized.replace('/', '\\');
  if (!normalized.startsWith('\\')) {
    normalized.prepend('\\');
  }
  return normalized;
}

auto QStringToFsPath(const QString& text) -> std::filesystem::path {
#if defined(_WIN32)
  return std::filesystem::path(text.toStdWString());
#else
  return std::filesystem::path(text.toStdString());
#endif
}

auto DateFromTimeT(std::time_t value) -> QDate {
  if (value <= 0) {
    return {};
  }

  std::tm tm{};
#if defined(_WIN32)
  if (localtime_s(&tm, &value) != 0) {
    return {};
  }
#else
  if (localtime_r(&value, &tm) == nullptr) {
    return {};
  }
#endif

  const QDate date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return date.isValid() ? date : QDate();
}

auto DateFromExifString(const std::string& value) -> QDate {
  if (value.empty()) {
    return {};
  }

  const QString text = QString::fromUtf8(value.c_str()).trimmed();
  const QDateTime dt = QDateTime::fromString(text, "yyyy-MM-dd HH:mm:ss");
  if (dt.isValid()) {
    return dt.date();
  }
  const QDate date = QDate::fromString(text, "yyyy-MM-dd");
  return date.isValid() ? date : QDate();
}

auto ExtensionUpper(const std::filesystem::path& path) -> QString {
  const QString ext = PathToQString(path.extension());
  if (ext.isEmpty()) {
    return QString();
  }
  return ext.startsWith('.') ? ext.mid(1).toUpper() : ext.toUpper();
}

auto ExtensionFromFileName(const QString& name) -> QString {
  const int dot = name.lastIndexOf('.');
  if (dot < 0 || dot + 1 >= name.size()) {
    return QString();
  }
  return name.mid(dot + 1).toUpper();
}

auto DataUrlFromImage(const QImage& image) -> QString {
  if (image.isNull()) {
    return QString();
  }

  QByteArray bytes;
  QBuffer    buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly)) {
    return QString();
  }
  if (!image.save(&buffer, "PNG")) {
    return QString();
  }
  return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

auto MatRgba32fToQImageCopy(const cv::Mat& rgba32fOrU8) -> QImage {
  if (rgba32fOrU8.empty()) {
    return {};
  }

  cv::Mat src8;
  if (rgba32fOrU8.depth() == CV_8U) {
    src8 = rgba32fOrU8;
  } else if (rgba32fOrU8.depth() == CV_32F) {
    rgba32fOrU8.convertTo(src8, CV_MAKETYPE(CV_8U, rgba32fOrU8.channels()), 255.0);
  } else {
    rgba32fOrU8.convertTo(src8, CV_MAKETYPE(CV_8U, rgba32fOrU8.channels()));
  }

  cv::Mat rgba8;
  switch (src8.channels()) {
    case 1:
      cv::cvtColor(src8, rgba8, cv::COLOR_GRAY2RGBA);
      break;
    case 3:
      if (rgba32fOrU8.depth() == CV_32F) {
        cv::cvtColor(src8, rgba8, cv::COLOR_RGB2RGBA);
      } else {
        cv::cvtColor(src8, rgba8, cv::COLOR_BGR2RGBA);
      }
      break;
    case 4:
      rgba8 = src8;
      break;
    default:
      return {};
  }

  if (!rgba8.isContinuous()) {
    rgba8 = rgba8.clone();
  }

  QImage img(rgba8.data, rgba8.cols, rgba8.rows, static_cast<int>(rgba8.step),
             QImage::Format_RGBA8888);
  return img.copy();
}

auto ExtensionForExportFormat(ImageFormatType format) -> std::string {
  switch (format) {
    case ImageFormatType::JPEG:
      return ".jpg";
    case ImageFormatType::PNG:
      return ".png";
    case ImageFormatType::TIFF:
      return ".tiff";
    case ImageFormatType::EXR:
      return ".exr";
    case ImageFormatType::WEBP:
    case ImageFormatType::BMP:
      // Deprecated export formats — callers should not reach here after validation.
      return {};
    default:
      return ".jpg";
  }
}

auto FormatFromName(const QString& value) -> ImageFormatType {
  const QString upper = value.trimmed().toUpper();
  if (upper == "PNG") {
    return ImageFormatType::PNG;
  }
  if (upper == "TIFF") {
    return ImageFormatType::TIFF;
  }
  if (upper == "EXR") {
    return ImageFormatType::EXR;
  }
  // WEBP / BMP export is deprecated; map any leftover UI/settings values to JPEG.
  return ImageFormatType::JPEG;
}

auto HdrExportModeFromName(const QString& value) -> ExportFormatOptions::HDR_EXPORT_MODE {
  const QString upper = value.trimmed().toUpper();
  if (upper == "EMBEDDED_PROFILE_ONLY") {
    return ExportFormatOptions::HDR_EXPORT_MODE::EMBEDDED_PROFILE_ONLY;
  }
  return ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;
}

auto BitDepthFromInt(int value) -> ExportFormatOptions::BIT_DEPTH {
  if (value == 8) {
    return ExportFormatOptions::BIT_DEPTH::BIT_8;
  }
  if (value == 32) {
    return ExportFormatOptions::BIT_DEPTH::BIT_32;
  }
  return ExportFormatOptions::BIT_DEPTH::BIT_16;
}

auto TiffCompressFromName(const QString& value) -> ExportFormatOptions::TIFF_COMPRESS {
  const QString upper = value.trimmed().toUpper();
  if (upper == "LZW") {
    return ExportFormatOptions::TIFF_COMPRESS::LZW;
  }
  if (upper == "ZIP") {
    return ExportFormatOptions::TIFF_COMPRESS::ZIP;
  }
  return ExportFormatOptions::TIFF_COMPRESS::NONE;
}

auto ExportPathForOptions(const std::filesystem::path& srcPath,
                          const std::filesystem::path& outDir,
                          sl_element_id_t elementId, image_id_t imageId,
                          ImageFormatType format) -> std::filesystem::path {
  (void)elementId;
  (void)imageId;

  std::wstring stem = srcPath.stem().wstring();
  if (stem.empty()) {
    stem = L"image";
  }
  const std::string ext = ExtensionForExportFormat(format);
  return outDir / (stem + std::wstring(ext.begin(), ext.end()));
}

auto ListCubeLutsInDir(const std::filesystem::path& dir) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
  std::error_code                    ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return files;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto ext = entry.path().extension().wstring();
    std::wstring normalized = ext;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    if (normalized == L".cube") {
      files.push_back(entry.path());
    }
  }

  std::sort(files.begin(), files.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.filename().wstring() < b.filename().wstring();
            });
  return files;
}

auto NearlyEqual(float a, float b) -> bool {
  return std::abs(a - b) <= 1e-6f;
}

auto ClampToRange(double value, double minValue, double maxValue) -> float {
  return static_cast<float>(std::clamp(value, minValue, maxValue));
}

auto AccentForIndex(size_t index) -> QString {
  return QString::fromLatin1(kThumbnailAccentPalette[index % kThumbnailAccentPalette.size()]);
}

auto ExportTargetKey(sl_element_id_t elementId, image_id_t imageId) -> uint64_t {
  return (static_cast<uint64_t>(elementId) << 32U) | static_cast<uint64_t>(imageId);
}

auto EscapeSqlStringLiteral(const std::string& text) -> std::string {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const char ch : text) {
    if (ch == '\'') {
      escaped.push_back('\'');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

auto EscapeSqlIdentifier(const std::string& text) -> std::string {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const char ch : text) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

auto ValidateProjectName(const QString& rawName, QString* errorOut) -> std::optional<QString> {
  const QString trimmed = rawName.trimmed();
  if (trimmed.isEmpty()) {
    if (errorOut) {
      *errorOut = Tr("Project name cannot be empty.");
    }
    return std::nullopt;
  }

  if (trimmed == "." || trimmed == "..") {
    if (errorOut) {
      *errorOut = Tr("Project name is invalid.");
    }
    return std::nullopt;
  }

  static const QString kInvalidChars = "<>:\"/\\|?*";
  for (const QChar ch : trimmed) {
    if (ch.unicode() < 32U || kInvalidChars.contains(ch)) {
      if (errorOut) {
        *errorOut = Tr("Project name contains invalid characters.");
      }
      return std::nullopt;
    }
  }

  if (trimmed.endsWith(' ') || trimmed.endsWith('.')) {
    if (errorOut) {
      *errorOut = Tr("Project name cannot end with a space or period.");
    }
    return std::nullopt;
  }

  return trimmed;
}

void CleanupWorkspaceDirectory(const std::filesystem::path& dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

auto EnsureDirectoryExists(const std::filesystem::path& dir) -> bool {
  if (dir.empty()) {
    return true;
  }
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    std::filesystem::create_directories(dir, ec);
  }
  if (ec) {
    return false;
  }
  return std::filesystem::is_directory(dir, ec) && !ec;
}

}  // namespace alcedo::ui::album_util
