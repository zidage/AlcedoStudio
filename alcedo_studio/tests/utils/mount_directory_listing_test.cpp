//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Directory listing of a mounted folder (mounted_folder_plan.md, MF2).

#include "utils/file/mount_directory_listing.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "../support/test_temp_directory.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#endif

namespace alcedo {
namespace {

namespace stdfs = std::filesystem;

void WriteText(const stdfs::path& path, const std::string& text) {
  stdfs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  ASSERT_TRUE(out.good()) << path.string();
}

auto RelativePaths(const std::vector<MountDirectoryFile>& files) -> std::vector<std::wstring> {
  std::vector<std::wstring> paths;
  for (const auto& file : files) paths.push_back(file.relative_path);
  return paths;
}

/// A directory link of the platform: an NTFS junction on Windows, a symbolic link elsewhere.
/// A junction needs no privilege, so the link is always possible on Windows.
auto CreateDirectoryLink(const stdfs::path& link, const stdfs::path& target) -> bool {
#ifdef _WIN32
  std::error_code ec;
  stdfs::create_directory(link, ec);
  if (ec) return false;
  const HANDLE handle =
      ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  // REPARSE_DATA_BUFFER.MountPointReparseBuffer: the substitute name `\??\<target>`, then the
  // print name `<target>`, each with a terminator.
  const std::wstring substitute = L"\\??\\" + stdfs::absolute(target).wstring();
  const std::wstring print      = stdfs::absolute(target).wstring();
  const auto         path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::vector<std::uint8_t> buffer(16 + path_bytes, 0);
  const auto                put16 = [&](std::size_t at, std::size_t value) {
    const auto v   = static_cast<std::uint16_t>(value);
    buffer[at]     = static_cast<std::uint8_t>(v & 0xFF);
    buffer[at + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, sizeof(tag));
  put16(4, 8 + path_bytes);                              // ReparseDataLength
  put16(8, 0);                                           // SubstituteNameOffset
  put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
  std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
              (print.size() + 1) * sizeof(wchar_t));
  DWORD      returned = 0;
  const BOOL ok =
      ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
  ::CloseHandle(handle);
  return ok != FALSE;
#else
  std::error_code ec;
  stdfs::create_directory_symlink(target, link, ec);
  return !ec;
#endif
}

/// Remove a directory link without an entry into its target.
void RemoveDirectoryLink(const stdfs::path& link) {
#ifdef _WIN32
  ::RemoveDirectoryW(link.c_str());
#else
  std::error_code ec;
  stdfs::remove(link, ec);
#endif
}

TEST(MountDirectoryListingTest, MissingRootIsUnreachableAndEmptyRootIsEmpty) {
  test::TestTempDirectory dir;

  const auto              missing = ListMountDirectory(dir / "absent");
  EXPECT_EQ(missing.status, MountDirectoryListingStatus::kRootUnreachable);
  EXPECT_FALSE(missing.IsComplete());
  EXPECT_TRUE(missing.files.empty());
  EXPECT_TRUE(missing.directories.empty());

  WriteText(dir / "file_root.dng", "x");
  EXPECT_EQ(ListMountDirectory(dir / "file_root.dng").status,
            MountDirectoryListingStatus::kRootUnreachable);
  EXPECT_EQ(ListMountDirectory(stdfs::path{}).status,
            MountDirectoryListingStatus::kRootUnreachable);

  stdfs::create_directories(dir / "empty");
  const auto empty = ListMountDirectory(dir / "empty");
  EXPECT_EQ(empty.status, MountDirectoryListingStatus::kComplete);
  EXPECT_TRUE(empty.files.empty());
  EXPECT_TRUE(empty.directories.empty());
  EXPECT_TRUE(empty.unreadable_directories.empty());
}

TEST(MountDirectoryListingTest, UnsupportedFilesAndPeriodDirectoriesAreAbsent) {
  test::TestTempDirectory dir;
  const auto              root = dir / "root";
  WriteText(root / "a.ARW", "raw");
  WriteText(root / "b.jpg", "jpeg!");
  WriteText(root / "notes.txt", "text");
  WriteText(root / "no_extension", "x");
  WriteText(root / ".hidden" / "c.dng", "hidden");
  WriteText(root / "sub" / "e.NEF", "nef");
  WriteText(root / "sub" / "._e.NEF", "apple double");
  WriteText(root / "sub" / "deep" / "g.tif", "tiff");
  WriteText(root / "sub" / "deep" / "h.exr", "exr");
  WriteText(root / "sub" / "deep" / "i.png", "png");
  stdfs::create_directories(root / "empty_dir");
  stdfs::create_directories(root / "folder.dng");

  const auto listing = ListMountDirectory(root);
  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_EQ(listing.directories,
            (std::vector<std::wstring>{L"empty_dir", L"folder.dng", L"sub", L"sub/deep"}));
  EXPECT_EQ(RelativePaths(listing.files),
            (std::vector<std::wstring>{L"a.ARW", L"b.jpg", L"sub/deep/g.tif", L"sub/deep/h.exr",
                                       L"sub/deep/i.png", L"sub/e.NEF"}));
  EXPECT_EQ(listing.files[0].byte_size, 3u);
  EXPECT_EQ(listing.files[1].byte_size, 5u);
  EXPECT_TRUE(listing.unreadable_directories.empty());
}

TEST(MountDirectoryListingTest, ModificationTimeIsNanosecondsSinceTheUnixEpoch) {
  test::TestTempDirectory dir;
  const auto              path = dir / "photo.cr2";
  WriteText(path, "cr2");
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  const auto written = ListMountDirectory(dir.Path());
  ASSERT_EQ(written.files.size(), 1u);
  constexpr std::int64_t kMinuteNs = 60LL * 1'000'000'000LL;
  EXPECT_LT(std::abs(written.files[0].modified_ns - now_ns), kMinuteNs);

  // A change of exactly one hour is a change of exactly 3600 s in the listing.
  stdfs::last_write_time(path, stdfs::last_write_time(path) - std::chrono::hours(1));
  const auto earlier = ListMountDirectory(dir.Path());
  ASSERT_EQ(earlier.files.size(), 1u);
  EXPECT_EQ(written.files[0].modified_ns - earlier.files[0].modified_ns, 3600LL * 1'000'000'000LL);
}

TEST(MountDirectoryListingTest, NamesAreInNormalizationFormC) {
  test::TestTempDirectory dir;
  // "Café" and "é.dng" written with a combining acute accent (normalization form D).
  WriteText(dir.Path() / stdfs::path(L"Café") / stdfs::path(L"é.dng"), "x");

  const auto listing = ListMountDirectory(dir.Path());
  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_EQ(listing.directories, (std::vector<std::wstring>{L"Café"}));
  EXPECT_EQ(RelativePaths(listing.files), (std::vector<std::wstring>{L"Café/é.dng"}));
}

// Mounted Folder Plan, MF4: the nested mount rule and the adoption input compare whole path
// elements.
TEST(MountDirectoryListingTest, PathAtOrBelowComparesWholeElements) {
  const stdfs::path photos = stdfs::path(L"/volume/Photos");
  EXPECT_TRUE(IsPathAtOrBelow(photos, photos));
  EXPECT_TRUE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos/"), photos));
  EXPECT_TRUE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos/Day1/a.jpg"), photos));
  EXPECT_TRUE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos/Day1/../Day2"), photos));
  EXPECT_FALSE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos2"), photos));
  EXPECT_FALSE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos2/a.jpg"), photos));
  EXPECT_FALSE(IsPathAtOrBelow(stdfs::path(L"/volume"), photos));
  EXPECT_FALSE(IsPathAtOrBelow(stdfs::path(L"/volume/Photos/.."), photos));
#ifdef _WIN32
  EXPECT_TRUE(IsPathAtOrBelow(stdfs::path(L"d:/photos/Day1"), stdfs::path(L"D:\\Photos")));
#else
  EXPECT_FALSE(IsPathAtOrBelow(stdfs::path(L"/volume/photos/Day1"), photos));
#endif
}

TEST(MountDirectoryListingTest, RelativePathOfAStoredPathHasTheListingForm) {
  const stdfs::path root = stdfs::path(L"/volume/Photos");
  EXPECT_EQ(MountRelativePathOf(root, root / L"Day1" / L"a.jpg"), std::wstring(L"Day1/a.jpg"));
  EXPECT_EQ(MountRelativePathOf(root, root / L"a.jpg"), std::wstring(L"a.jpg"));
  // Names with a combining acute accent (normalization form D) give the NFC names of a listing.
  EXPECT_EQ(MountRelativePathOf(root, root / L"Café" / L"é.dng"), std::wstring(L"Café/é.dng"));
  EXPECT_EQ(MountRelativePathOf(root, root), std::nullopt);
  EXPECT_EQ(MountRelativePathOf(root, stdfs::path(L"/volume/Photos2/a.jpg")), std::nullopt);
  EXPECT_EQ(MountRelativePathOf(root, stdfs::path(L"/volume/a.jpg")), std::nullopt);
}

TEST(MountDirectoryListingTest, OneFileReadGivesTheEntryOfTheListing) {
  test::TestTempDirectory dir;
  const auto              root = dir / "root";
  WriteText(root / "sub" / "deep" / "g.tif", "tiff");
  WriteText(root / "b.jpg", "jpeg!");
  WriteText(root / "notes.txt", "text");
  WriteText(root / ".hidden" / "c.dng", "hidden");
  WriteText(dir / "outside.jpg", "outside");

  // Locate of a missing photo (MF8) stores the values that a scan stores for the file.
  const auto listing = ListMountDirectory(root);
  ASSERT_EQ(listing.files.size(), 2u);
  for (const auto& listed : listing.files) {
    SCOPED_TRACE(listed.relative_path);
    stdfs::path path = root;
    for (const auto& element : stdfs::path(listed.relative_path)) {
      path /= element;
    }
    const auto file = ReadMountDirectoryFile(root, path);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(*file, listed);
  }

  EXPECT_FALSE(ReadMountDirectoryFile(root, root / "notes.txt").has_value());
  EXPECT_FALSE(ReadMountDirectoryFile(root, root / ".hidden" / "c.dng").has_value());
  EXPECT_FALSE(ReadMountDirectoryFile(root, root / "absent.jpg").has_value());
  EXPECT_FALSE(ReadMountDirectoryFile(root, root / "sub").has_value());
  EXPECT_FALSE(ReadMountDirectoryFile(root, root).has_value());
  EXPECT_FALSE(ReadMountDirectoryFile(root, dir / "outside.jpg").has_value());
}

TEST(MountDirectoryListingTest, DirectoryLinkCycleEnds) {
  test::TestTempDirectory dir;
  const auto              root = dir / "root";
  WriteText(root / "a.dng", "a");
  WriteText(root / "sub" / "b.dng", "b");
  const auto link = root / "sub" / "loop";
  if (!CreateDirectoryLink(link, root)) {
    GTEST_SKIP() << "This account cannot create a directory link.";
  }

  const auto listing = ListMountDirectory(root);
  RemoveDirectoryLink(link);
  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_EQ(listing.directories, (std::vector<std::wstring>{L"sub"}));
  EXPECT_EQ(RelativePaths(listing.files), (std::vector<std::wstring>{L"a.dng", L"sub/b.dng"}));
}

TEST(MountDirectoryListingTest, SymbolicLinksToFilesAndDirectoriesAreAbsent) {
  test::TestTempDirectory dir;
  const auto              root = dir / "root";
  WriteText(dir / "outside" / "outside.dng", "o");
  WriteText(root / "inside.dng", "i");
  std::error_code ec;
  stdfs::create_symlink(dir / "outside" / "outside.dng", root / "file_link.dng", ec);
  if (ec) {
    GTEST_SKIP() << "This account cannot create a symbolic link: " << ec.message();
  }
  stdfs::create_directory_symlink(dir / "outside", root / "directory_link", ec);
  ASSERT_FALSE(ec) << ec.message();

  const auto listing = ListMountDirectory(root);
  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_TRUE(listing.directories.empty());
  EXPECT_EQ(RelativePaths(listing.files), (std::vector<std::wstring>{L"inside.dng"}));
  RemoveDirectoryLink(root / "directory_link");
}

TEST(MountDirectoryListingTest, StopRequestEndsTheListingWithNoEntries) {
  test::TestTempDirectory dir;
  WriteText(dir / "a" / "x.dng", "x");
  const std::atomic<bool> stop{true};

  const auto              listing = ListMountDirectory(dir.Path(), &stop);
  EXPECT_EQ(listing.status, MountDirectoryListingStatus::kStopped);
  EXPECT_TRUE(listing.files.empty());
  EXPECT_TRUE(listing.directories.empty());
}

#ifndef _WIN32
TEST(MountDirectoryListingTest, UnreadableSubdirectoryIsReportedAndTheRestIsListed) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "The root account reads a directory without read permission.";
  }
  test::TestTempDirectory dir;
  WriteText(dir / "open" / "a.dng", "a");
  WriteText(dir / "closed" / "b.dng", "b");
  stdfs::permissions(dir / "closed", stdfs::perms::none);

  const auto listing = ListMountDirectory(dir.Path());
  stdfs::permissions(dir / "closed", stdfs::perms::owner_all);
  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_EQ(listing.directories, (std::vector<std::wstring>{L"closed", L"open"}));
  EXPECT_EQ(RelativePaths(listing.files), (std::vector<std::wstring>{L"open/a.dng"}));
  EXPECT_EQ(listing.unreadable_directories, (std::vector<std::wstring>{L"closed"}));
}
#endif

TEST(MountDirectoryListingTest, TenThousandFilesAreListedWhileNoFileCanBeOpened) {
  // Each file is locked against every other open (Windows: a handle with no share flags;
  // POSIX: no read permission). The listing still returns each file with its byte size, so it
  // reads directory entries and file attributes only.
  test::TestTempDirectory  dir;
  constexpr int            kDirectories        = 100;
  constexpr int            kFilesEachDirectory = 100;
  std::vector<stdfs::path> paths;
  paths.reserve(kDirectories * kFilesEachDirectory);
  for (int d = 0; d < kDirectories; ++d) {
    const auto directory = dir / std::format("d{:03}", d);
    stdfs::create_directories(directory);
    for (int f = 0; f < kFilesEachDirectory; ++f) {
      paths.push_back(directory / std::format("f{:03}.dng", f));
      std::ofstream out(paths.back(), std::ios::binary);
      out << std::string(static_cast<std::size_t>(f + 1), 'x');
    }
  }

#ifdef _WIN32
  std::vector<HANDLE> locks;
  locks.reserve(paths.size());
  for (const auto& path : paths) {
    const HANDLE lock = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    locks.push_back(lock);
  }
  const HANDLE probe = ::CreateFileW(paths.front().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_EQ(probe, INVALID_HANDLE_VALUE) << "The lock does not prevent a read open.";
#else
  for (const auto& path : paths) stdfs::permissions(path, stdfs::perms::none);
#endif

  const auto listing = ListMountDirectory(dir.Path());

#ifdef _WIN32
  for (const HANDLE lock : locks) ::CloseHandle(lock);
#else
  for (const auto& path : paths) stdfs::permissions(path, stdfs::perms::owner_all);
#endif

  ASSERT_EQ(listing.status, MountDirectoryListingStatus::kComplete);
  EXPECT_TRUE(listing.unreadable_directories.empty());
  EXPECT_EQ(listing.directories.size(), static_cast<std::size_t>(kDirectories));
  ASSERT_EQ(listing.files.size(), paths.size());
  for (std::size_t i = 0; i < listing.files.size(); ++i) {
    const int f = static_cast<int>(i % kFilesEachDirectory);
    EXPECT_EQ(listing.files[i].relative_path,
              std::format(L"d{:03}/f{:03}.dng", static_cast<int>(i / kFilesEachDirectory), f));
    EXPECT_EQ(listing.files[i].byte_size, static_cast<std::uint64_t>(f + 1));
  }
}

}  // namespace
}  // namespace alcedo
