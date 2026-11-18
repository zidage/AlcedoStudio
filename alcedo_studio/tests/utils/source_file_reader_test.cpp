// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "utils/file/source_file_reader.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace alcedo {
namespace {
auto ReaderTestDirectory() -> QString {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
      "build/tmp/source_file_reader";
  std::filesystem::create_directories(root);
  return QString::fromStdWString((root / "case-XXXXXX").wstring());
}
TEST(SourceFileReaderTest, ReportsFileSizeWithoutReadingSourceContents) {
  QTemporaryDir directory(ReaderTestDirectory());
  ASSERT_TRUE(directory.isValid());
  const auto path = std::filesystem::path(directory.path().toStdWString()) / "source.raw";
  std::ofstream(path, std::ios::binary) << "abc";
  std::filesystem::resize_file(path, 35 * 1024 * 1024);
#ifdef _WIN32
  IO_COUNTERS before{}, after{};
  ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
#endif
  SourceFileReader reader(path);
  EXPECT_EQ(reader.Size(), 35 * 1024 * 1024);
  reader.Validate();
#ifdef _WIN32
  ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
  EXPECT_EQ(after.ReadTransferCount, before.ReadTransferCount);
#endif
}

TEST(SourceFileReaderTest, MissingSourceReportsTheReadFailure) {
  QTemporaryDir directory(ReaderTestDirectory());
  ASSERT_TRUE(directory.isValid());
  const auto path = std::filesystem::path(directory.path().toStdWString()) / "missing.raw";
  try {
    SourceFileReader reader(path);
    FAIL() << "Missing source was accepted";
  } catch (const SourceReadError& error) {
    EXPECT_EQ(error.code, ImportErrorCode::READ_FAILED);
  }
}

TEST(SourceFileReaderTest, EmptyReadableFileHasZeroBytes) {
  QTemporaryDir directory(ReaderTestDirectory());
  ASSERT_TRUE(directory.isValid());
  const auto path = std::filesystem::path(directory.path().toStdWString()) / "empty.raw";
  std::ofstream(path, std::ios::binary).close();
  SourceFileReader reader(path);
  EXPECT_EQ(reader.Size(), 0);
  reader.Validate();
}

TEST(SourceFileReaderTest, KeepsMetadataReopenReadableAndProtectsTheSourceUntilPublication) {
  QTemporaryDir directory(ReaderTestDirectory());
  ASSERT_TRUE(directory.isValid());
  const auto path = std::filesystem::path(directory.path().toStdWString()) / "source.raw";
  std::ofstream(path, std::ios::binary) << "abc";
  SourceFileReader reader(path);
  std::ifstream    metadata(path, std::ios::binary);
  EXPECT_EQ(metadata.get(), 'a');
#ifdef _WIN32
  const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
  std::error_code error;
  std::filesystem::rename(path, path.parent_path() / "replaced.raw", error);
  EXPECT_TRUE(error);
  reader.Validate();
#else
  std::ofstream(path, std::ios::binary | std::ios::app) << 'd';
  EXPECT_THROW(reader.Validate(), SourceReadError);
#endif
}
}  // namespace
}  // namespace alcedo
