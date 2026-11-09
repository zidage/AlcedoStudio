//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/embedded_preview_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {
namespace {

auto SampleRoot() -> std::filesystem::path { return std::filesystem::path(TEST_IMG_PATH); }

auto IsLfsPointer(const std::filesystem::path& path) -> bool {
  std::ifstream file(path, std::ios::binary);
  char          magic[32] = {0};
  file.read(magic, sizeof(magic) - 1);
  return std::string_view(magic).starts_with("version https://git-lfs");
}

/// The RAW files of the CI fixture set with the extension @p extension (lower case).
auto CiRawFiles(std::string_view extension) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  const auto                         root = SampleRoot() / "ci_rawfiles";
  if (!std::filesystem::exists(root)) {
    return paths;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == extension && !IsLfsPointer(entry.path())) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

auto FirstJpegSample() -> std::filesystem::path {
  const auto root = SampleRoot() / "jpeg";
  if (!std::filesystem::exists(root)) {
    return {};
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    const auto ext = entry.path().extension().string();
    if (entry.is_regular_file() && (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg") &&
        !IsLfsPointer(entry.path())) {
      return entry.path();
    }
  }
  return {};
}

}  // namespace

// The Sony ARW fixtures carry a large JPEG preview: it decodes to RGB8 at a reduced scale whose
// shorter edge is the requested size or larger, and no larger than the next scale step needs.
TEST(EmbeddedPreviewReaderTest, ArwPreviewDecodesAtTheSmallestScaleThatKeepsTheShortEdge) {
  const auto files = CiRawFiles(".arw");
  if (files.empty()) {
    GTEST_SKIP() << "ARW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  constexpr uint32_t kMinShortEdge = 256;
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    const auto preview = EmbeddedPreviewReader::Read(path, kMinShortEdge);
    ASSERT_TRUE(preview.has_value());
    EXPECT_EQ(preview->format, EmbeddedPreviewFormat::kJpeg);
    EXPECT_EQ(preview->rgb8.type(), CV_8UC3);
    const int short_edge = std::min(preview->rgb8.cols, preview->rgb8.rows);
    EXPECT_GE(short_edge, static_cast<int>(kMinShortEdge));
    const auto stored_short = std::min(preview->stored_width, preview->stored_height);
    // The preview decoded at full size, at TurboJPEG's smallest scale (1/8), or at a scale where
    // one more halving would fall below the request.
    const auto eighth_short = (stored_short + 7) / 8;
    EXPECT_TRUE(static_cast<uint32_t>(short_edge) == stored_short ||
                static_cast<uint32_t>(short_edge) == eighth_short ||
                short_edge < static_cast<int>(2 * kMinShortEdge) + 2)
        << "short edge " << short_edge << " of stored " << stored_short;
  }
}

// The preview pixels are upright: a 90 degree RAW flip swaps the stored width and height, and the
// other flips keep them.
TEST(EmbeddedPreviewReaderTest, PreviewExtentFollowsTheRawOrientation) {
  const auto files = CiRawFiles(".arw");
  if (files.empty()) {
    GTEST_SKIP() << "ARW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    const auto preview = EmbeddedPreviewReader::Read(path, 64);
    ASSERT_TRUE(preview.has_value());
    const bool stored_landscape = preview->stored_width >= preview->stored_height;
    const bool shown_landscape  = preview->rgb8.cols >= preview->rgb8.rows;
    const bool quarter_turn     = preview->orientation_flip == 5 || preview->orientation_flip == 6;
    EXPECT_EQ(shown_landscape, quarter_turn ? !stored_landscape : stored_landscape);
  }
}

// A preview smaller than the request is no preview for that request.
TEST(EmbeddedPreviewReaderTest, ReturnsNothingWhenThePreviewIsSmallerThanTheRequest) {
  const auto files = CiRawFiles(".arw");
  if (files.empty()) {
    GTEST_SKIP() << "ARW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  EXPECT_FALSE(EmbeddedPreviewReader::Read(files.front(), 100000).has_value());
}

// DNG fixtures may or may not store a preview; a read never fails on them, and a returned preview
// meets the request.
TEST(EmbeddedPreviewReaderTest, DngPreviewIsReadOrAbsentWithoutError) {
  const auto files = CiRawFiles(".dng");
  if (files.empty()) {
    GTEST_SKIP() << "DNG fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    const auto preview = EmbeddedPreviewReader::Read(path, 256);
    if (preview.has_value()) {
      EXPECT_GE(std::min(preview->rgb8.cols, preview->rgb8.rows), 256);
    }
  }
}

// A DNG that Lightroom merged from several frames (HDR or panorama) stores a full-size
// JPEG-compressed transparency mask next to the colour preview. The preview is the colour image.
// The files are hundreds of MB, so the test runs when ALCEDO_TEST_MERGED_DNG names one.
TEST(EmbeddedPreviewReaderTest, MergedDngPreviewIsTheColourImageAndNotTheTransparencyMask) {
#ifdef _WIN32
  const wchar_t* merged_dng = _wgetenv(L"ALCEDO_TEST_MERGED_DNG");
#else
  const char* merged_dng = std::getenv("ALCEDO_TEST_MERGED_DNG");
#endif
  if (merged_dng == nullptr || *merged_dng == 0) {
    GTEST_SKIP() << "set ALCEDO_TEST_MERGED_DNG to a DNG merged in Lightroom";
  }
  const std::filesystem::path path(merged_dng);
  const auto                  preview = EmbeddedPreviewReader::Read(path, 256);
  ASSERT_TRUE(preview.has_value());
  ASSERT_EQ(preview->rgb8.type(), CV_8UC3);
  EXPECT_GE(std::min(preview->rgb8.cols, preview->rgb8.rows), 256);
  // The mask decodes to grey: its three channels are equal on every pixel.
  std::vector<cv::Mat> channels;
  cv::split(preview->rgb8, channels);
  EXPECT_GT(cv::norm(channels[0], channels[1], cv::NORM_L1) +
                cv::norm(channels[1], channels[2], cv::NORM_L1),
            0.0);
}

// A JPEG file is not a RAW file: LibRaw does not open it, so it has no embedded preview.
TEST(EmbeddedPreviewReaderTest, ReturnsNothingForAJpegFile) {
  const auto jpeg = FirstJpegSample();
  if (jpeg.empty()) {
    GTEST_SKIP() << "JPEG fixtures missing under TEST_IMG_PATH/jpeg";
  }
  EXPECT_FALSE(EmbeddedPreviewReader::Read(jpeg, 256).has_value());
}

TEST(EmbeddedPreviewReaderTest, MissingFileThrowsWithThePath) {
  const auto missing = SampleRoot() / "ci_rawfiles" / "missing_embedded_preview_file.ARW";
  try {
    (void)EmbeddedPreviewReader::Read(missing, 256);
    FAIL() << "a missing file must throw";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("missing_embedded_preview_file"), std::string::npos)
        << e.what();
  }
}

}  // namespace alcedo
