//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Fingerprint version 1 of a source file (mounted_folder_plan.md, MF2): XXH3-128 of the byte size,
// the first 128 KiB, and the last 128 KiB.

#include "utils/file/source_file_fingerprint.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../support/test_temp_directory.hpp"
#include "type/hash_type.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace alcedo {
namespace {

constexpr std::size_t kBlock = kSourceFileFingerprintBlockSize;

/// Bytes of a fixed pseudo-random sequence, so that each block of a test file is different.
auto PatternBytes(std::size_t size, std::uint32_t start = 0x1234567u) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(size);
  std::uint32_t          state = start;
  for (auto& byte : bytes) {
    state = state * 1664525u + 1013904223u;
    byte  = static_cast<std::byte>(state >> 24);
  }
  return bytes;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path.string();
}

auto FingerprintOf(const std::filesystem::path& path) -> SourceFileFingerprint {
  const auto result = ComputeSourceFileFingerprint(path);
  EXPECT_TRUE(result.HasFingerprint()) << path.string();
  EXPECT_EQ(result.fingerprint.version, 1u);
  return result.fingerprint;
}

/// The fingerprint value that the definition of version 1 gives for @p bytes, computed with the
/// one-shot XXH3-128 of the concatenated input.
auto ExpectedHash(const std::vector<std::byte>& bytes) -> Hash128 {
  std::vector<std::byte> input(8);
  const std::uint64_t    size = bytes.size();
  for (std::size_t i = 0; i < 8; ++i) input[i] = static_cast<std::byte>((size >> (8 * i)) & 0xFF);
  if (bytes.size() <= 2 * kBlock) {
    input.insert(input.end(), bytes.begin(), bytes.end());
  } else {
    input.insert(input.end(), bytes.begin(), bytes.begin() + kBlock);
    input.insert(input.end(), bytes.end() - kBlock, bytes.end());
  }
  return Hash128::Compute(input.data(), input.size());
}

auto WithByteChanged(std::vector<std::byte> bytes, std::size_t offset) -> std::vector<std::byte> {
  bytes[offset] ^= std::byte{0x5A};
  return bytes;
}

TEST(SourceFileFingerprintTest, EqualContentAtTwoPathsHasEqualFingerprint) {
  test::TestTempDirectory dir;
  const auto              bytes = PatternBytes(1024 * 1024);
  WriteBytes(dir / "a" / "DSC0001.ARW", bytes);
  WriteBytes(dir / "b" / "renamed.ARW", bytes);

  const auto first  = FingerprintOf(dir / "a" / "DSC0001.ARW");
  const auto second = FingerprintOf(dir / "b" / "renamed.ARW");
  EXPECT_EQ(first, second);
  EXPECT_EQ(first.hash, ExpectedHash(bytes));
}

TEST(SourceFileFingerprintTest, ChangeInFirstOrLastBlockChangesFingerprint) {
  test::TestTempDirectory dir;
  const auto              bytes = PatternBytes(1024 * 1024);
  WriteBytes(dir / "base.dng", bytes);
  const auto base = FingerprintOf(dir / "base.dng");

  // The first byte, the last byte of the first block, the first byte of the last block, and the
  // last byte of the file.
  for (const std::size_t offset :
       {std::size_t{0}, kBlock - 1, bytes.size() - kBlock, bytes.size() - 1}) {
    const auto path = dir / ("changed_" + std::to_string(offset) + ".dng");
    WriteBytes(path, WithByteChanged(bytes, offset));
    EXPECT_NE(FingerprintOf(path), base) << "offset " << offset;
  }

  // One more byte at the end changes the byte size and the last block.
  auto longer = bytes;
  longer.push_back(std::byte{0});
  WriteBytes(dir / "longer.dng", longer);
  EXPECT_NE(FingerprintOf(dir / "longer.dng"), base);
}

TEST(SourceFileFingerprintTest, ChangeInTheMiddleOfALargeFileKeepsFingerprint) {
  // This records the limit of version 1: a byte outside the two blocks does not contribute.
  test::TestTempDirectory dir;
  const auto              bytes = PatternBytes(1024 * 1024);
  WriteBytes(dir / "base.dng", bytes);
  const auto base = FingerprintOf(dir / "base.dng");

  for (const std::size_t offset : {kBlock, bytes.size() / 2, bytes.size() - kBlock - 1}) {
    const auto path = dir / ("middle_" + std::to_string(offset) + ".dng");
    WriteBytes(path, WithByteChanged(bytes, offset));
    EXPECT_EQ(FingerprintOf(path), base) << "offset " << offset;
  }
}

TEST(SourceFileFingerprintTest, SmallFileIsHashedInFull) {
  test::TestTempDirectory dir;

  // Two blocks exactly: each byte contributes one time, so a byte in the middle counts.
  const auto              two_blocks = PatternBytes(2 * kBlock);
  WriteBytes(dir / "two_blocks.jpg", two_blocks);
  const auto two_blocks_fingerprint = FingerprintOf(dir / "two_blocks.jpg");
  EXPECT_EQ(two_blocks_fingerprint.hash, ExpectedHash(two_blocks));
  WriteBytes(dir / "two_blocks_changed.jpg", WithByteChanged(two_blocks, kBlock));
  EXPECT_NE(FingerprintOf(dir / "two_blocks_changed.jpg"), two_blocks_fingerprint);

  // One byte more than two blocks: the first and the last block only.
  const auto just_larger = PatternBytes(2 * kBlock + 1);
  WriteBytes(dir / "just_larger.jpg", just_larger);
  EXPECT_EQ(FingerprintOf(dir / "just_larger.jpg").hash, ExpectedHash(just_larger));

  // A small file and an empty file.
  const auto small = PatternBytes(1000);
  WriteBytes(dir / "small.png", small);
  EXPECT_EQ(FingerprintOf(dir / "small.png").hash, ExpectedHash(small));
  WriteBytes(dir / "empty.png", {});
  EXPECT_EQ(FingerprintOf(dir / "empty.png").hash, ExpectedHash({}));
}

TEST(SourceFileFingerprintTest, MissingFileOrDirectoryReportsCannotOpen) {
  test::TestTempDirectory dir;
  EXPECT_EQ(ComputeSourceFileFingerprint(dir / "absent.dng").error,
            SourceFileFingerprintError::kCannotOpen);
  EXPECT_EQ(ComputeSourceFileFingerprint(dir.Path()).error,
            SourceFileFingerprintError::kCannotOpen);
}

TEST(SourceFileFingerprintTest, ReadOnlyFileHasFingerprintAndKeepsItsModificationTime) {
  // The function opens the file for read only: a file without write permission has a
  // fingerprint, and the modification time does not change.
  test::TestTempDirectory dir;
  const auto              path  = dir / "read_only.nef";
  const auto              bytes = PatternBytes(3 * kBlock);
  WriteBytes(path, bytes);
  const auto written_time = std::filesystem::last_write_time(path);
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::others_read);

  const auto result = ComputeSourceFileFingerprint(path);
  EXPECT_TRUE(result.HasFingerprint());
  EXPECT_EQ(result.fingerprint.hash, ExpectedHash(bytes));
  EXPECT_EQ(std::filesystem::last_write_time(path), written_time);

  std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
}

#ifdef _WIN32
TEST(SourceFileFingerprintTest, FileLockedByAnotherHandleReportsCannotOpen) {
  test::TestTempDirectory dir;
  const auto              path = dir / "locked.cr3";
  WriteBytes(path, PatternBytes(1000));
  // No share flags: no other open of the file can succeed while the handle is open.
  const HANDLE lock = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(lock, INVALID_HANDLE_VALUE);
  EXPECT_EQ(ComputeSourceFileFingerprint(path).error, SourceFileFingerprintError::kCannotOpen);
  ::CloseHandle(lock);
  EXPECT_TRUE(ComputeSourceFileFingerprint(path).HasFingerprint());
}
#endif

}  // namespace
}  // namespace alcedo
