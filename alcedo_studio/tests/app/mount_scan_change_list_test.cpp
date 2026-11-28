//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// MF3 of the Mounted Folder Plan: BuildMountScanChangeList compares the records of one mount
// with a listing. Each test compares the complete change list with the expected list, and each
// test that names a read checks the paths that the fingerprint function received.

#include "app/mount_scan_change_list.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "type/hash_type.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/source_file_fingerprint.hpp"

namespace alcedo {

// gtest output of a change list that differs from the expected list.
void PrintTo(const MountScanChangeList& changes, std::ostream* out) {
  const auto text = [](const std::wstring& path) {
    std::string ascii;
    for (const wchar_t c : path) ascii.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return ascii;
  };
  const auto values = [](const MountScanSourceValues& v) {
    return " size=" + std::to_string(v.byte_size) + " time=" + std::to_string(v.modified_ns) +
           " fp=" + v.fingerprint.hash.ToString() + "/" + std::to_string(v.fingerprint.version);
  };
  *out << "{";
  for (const auto& u : changes.changed_in_place)
    *out << "\n changed " << u.element_id << " " << text(u.relative_path) << values(u.values);
  for (const auto& m : changes.moved)
    *out << "\n moved " << m.element_id << " " << text(m.previous_relative_path) << " -> "
         << text(m.relative_path) << values(m.values);
  for (const auto& n : changes.new_files)
    *out << "\n new " << text(n.relative_path) << values(n.values);
  for (const auto& n : changes.unread_new_files)
    *out << "\n unread new " << text(n.relative_path) << " size=" << n.byte_size
         << " time=" << n.modified_ns;
  for (const auto& m : changes.missing)
    *out << "\n missing " << m.element_id << " " << text(m.relative_path);
  for (const auto& u : changes.returned)
    *out << "\n returned " << u.element_id << " " << text(u.relative_path) << values(u.values);
  for (const auto& u : changes.adopted)
    *out << "\n adopted " << u.element_id << " " << text(u.relative_path) << values(u.values);
  for (const auto& f : changes.folders_to_add) *out << "\n add folder " << text(f);
  for (const auto& f : changes.folders_to_remove)
    *out << "\n remove folder " << f.element_id << " " << text(f.relative_path);
  *out << "\n unchanged=" << changes.unchanged_count
       << " still_missing=" << changes.still_missing_count
       << " unreadable=" << changes.unreadable_count << " unverified=" << changes.unverified_count
       << " deferred=" << changes.deferred_count << "\n}";
}

namespace {

constexpr std::uint64_t kSize = 25'000'000;
constexpr std::int64_t  kTime = 1'763'600'000'000'000'000;

auto                    Fingerprint(std::uint64_t content) -> SourceFileFingerprint {
  return SourceFileFingerprint{Hash128(content, 0), kSourceFileFingerprintVersion};
}

auto RecordOf(sl_element_id_t id, std::wstring path, std::uint64_t content, SourceFileState state,
              std::uint64_t size = kSize, std::int64_t time = kTime) -> MountScanFileRecord {
  SourceFileRecord source;
  source.element_id          = id;
  source.byte_size           = size;
  source.modified_ns         = time;
  source.fingerprint         = Fingerprint(content).hash;
  source.fingerprint_version = kSourceFileFingerprintVersion;
  source.state               = state;
  if (state == SourceFileState::kMissing) source.missing_since = 1'763'000'000;
  return MountScanFileRecord{id, std::move(path), source};
}

auto Present(sl_element_id_t id, std::wstring path, std::uint64_t content,
             std::uint64_t size = kSize, std::int64_t time = kTime) -> MountScanFileRecord {
  return RecordOf(id, std::move(path), content, SourceFileState::kPresent, size, time);
}

auto Missing(sl_element_id_t id, std::wstring path, std::uint64_t content) -> MountScanFileRecord {
  return RecordOf(id, std::move(path), content, SourceFileState::kMissing);
}

auto Values(std::uint64_t content, std::uint64_t size = kSize, std::int64_t time = kTime)
    -> MountScanSourceValues {
  return MountScanSourceValues{size, time, Fingerprint(content)};
}

/// A directory tree on a fake disk: each file has a content number, and the fingerprint
/// function returns the fingerprint of that number and records each path that it read.
class FakeDisk {
 public:
  void Add(std::wstring path, std::uint64_t content, std::uint64_t size = kSize,
           std::int64_t time = kTime) {
    files_[path] = File{size, time, content, false};
  }
  /// A file that is listed but cannot be read (another process holds it locked).
  void AddLocked(std::wstring path, std::uint64_t size = kSize, std::int64_t time = kTime) {
    files_[path] = File{size, time, 0, true};
  }
  void UnreadableDirectory(std::wstring path) { unreadable_.push_back(std::move(path)); }

  [[nodiscard]] auto Listing() const -> MountDirectoryListing {
    MountDirectoryListing listing;
    for (const auto& [path, file] : files_) {
      listing.files.push_back(MountDirectoryFile{path, file.size, file.time});
    }
    listing.unreadable_directories = unreadable_;
    return listing;
  }

  [[nodiscard]] auto Function() -> MountScanFingerprintFunction {
    return [this](const MountDirectoryFile& listed) {
      reads_.push_back(listed.relative_path);
      const File& file = files_.at(listed.relative_path);
      if (file.locked) {
        return SourceFileFingerprintResult{SourceFileFingerprintError::kCannotOpen, {}};
      }
      return SourceFileFingerprintResult{SourceFileFingerprintError::kNone,
                                         Fingerprint(file.content)};
    };
  }

  [[nodiscard]] auto Reads() const -> const std::vector<std::wstring>& { return reads_; }

 private:
  struct File {
    std::uint64_t size;
    std::int64_t  time;
    std::uint64_t content;
    bool          locked;
  };
  std::map<std::wstring, File> files_;
  std::vector<std::wstring>    unreadable_;
  std::vector<std::wstring>    reads_;
};

auto Scan(const MountScanRecords& records, FakeDisk& disk) -> MountScanChangeList {
  return BuildMountScanChangeList(records, disk.Listing(), disk.Function());
}

/// A listed file that the comparison did not read.
auto Unread(std::wstring path, std::uint64_t size = kSize, std::int64_t time = kTime)
    -> MountDirectoryFile {
  return MountDirectoryFile{std::move(path), size, time};
}

using Paths = std::vector<std::wstring>;

}  // namespace

TEST(MountScanChangeListTest, UnchangedFileNeedsNoFingerprint) {
  FakeDisk disk;
  disk.Add(L"2026/a.ARW", 1);
  disk.Add(L"b.ARW", 2);
  const MountScanRecords records{
      .files   = {Present(10, L"2026/a.ARW", 1), Present(11, L"b.ARW", 2)},
      .folders = {{20, L"2026"}},
  };

  MountScanChangeList expected;
  expected.unchanged_count = 2;
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, FileWithNewModificationTimeIsChangedInPlace) {
  FakeDisk disk;
  disk.Add(L"a.JPG", 5, kSize, kTime + 1'000);
  disk.Add(L"b.JPG", 6, kSize + 4, kTime);
  const MountScanRecords records{.files = {Present(10, L"a.JPG", 1), Present(11, L"b.JPG", 2)}};

  MountScanChangeList    expected;
  expected.changed_in_place = {{10, L"a.JPG", Values(5, kSize, kTime + 1'000)},
                               {11, L"b.JPG", Values(6, kSize + 4, kTime)}};
  EXPECT_EQ(Scan(records, disk), expected);
  // The source record gets the fingerprint of the new content.
  EXPECT_EQ(disk.Reads(), (Paths{L"a.JPG", L"b.JPG"}));
}

TEST(MountScanChangeListTest, RenamedFileKeepsItsElement) {
  FakeDisk disk;
  disk.Add(L"trip/IMG_0001.ARW", 1);
  disk.Add(L"trip/beach.ARW", 2);
  const MountScanRecords records{
      .files   = {Present(10, L"trip/IMG_0001.ARW", 1), Present(11, L"trip/IMG_0002.ARW", 2)},
      .folders = {{20, L"trip"}},
  };

  MountScanChangeList expected;
  expected.moved           = {{11, L"trip/IMG_0002.ARW", L"trip/beach.ARW", Values(2)}};
  expected.unchanged_count = 1;
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{L"trip/beach.ARW"});
}

TEST(MountScanChangeListTest, RenamedDirectoryMovesEachFileAndReplacesTheFolder) {
  FakeDisk disk;
  disk.Add(L"2026/Iceland/a.ARW", 1);
  disk.Add(L"2026/Iceland/day 2/b.ARW", 2);
  const MountScanRecords records{
      .files   = {Present(10, L"2026/trip/a.ARW", 1), Present(11, L"2026/trip/day 2/b.ARW", 2)},
      .folders = {{20, L"2026"}, {21, L"2026/trip"}, {22, L"2026/trip/day 2"}},
  };

  MountScanChangeList expected;
  expected.moved             = {{10, L"2026/trip/a.ARW", L"2026/Iceland/a.ARW", Values(1)},
                                {11, L"2026/trip/day 2/b.ARW", L"2026/Iceland/day 2/b.ARW", Values(2)}};
  expected.folders_to_add    = {L"2026/Iceland", L"2026/Iceland/day 2"};
  expected.folders_to_remove = {{22, L"2026/trip/day 2"}, {21, L"2026/trip"}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, FileMovedBetweenDirectoriesKeepsItsElement) {
  FakeDisk disk;
  disk.Add(L"keep/a.ARW", 1);
  disk.Add(L"keep/b.ARW", 2);
  disk.Add(L"review/c.ARW", 3);
  const MountScanRecords records{
      .files   = {Present(10, L"review/a.ARW", 1), Present(11, L"keep/b.ARW", 2),
                  Present(12, L"review/c.ARW", 3)},
      .folders = {{20, L"keep"}, {21, L"review"}},
  };

  MountScanChangeList expected;
  expected.moved           = {{10, L"review/a.ARW", L"keep/a.ARW", Values(1)}};
  expected.unchanged_count = 2;
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, CopyOfAPresentFileIsANewFile) {
  // No record is lost, so no move can end at a copy: the comparison reads no file.
  FakeDisk disk;
  disk.Add(L"a.ARW", 1);
  disk.Add(L"a copy.ARW", 1);
  disk.Add(L"backup/a.ARW", 1);
  const MountScanRecords records{.files = {Present(10, L"a.ARW", 1)}};

  MountScanChangeList    expected;
  expected.unread_new_files = {Unread(L"a copy.ARW"), Unread(L"backup/a.ARW")};
  expected.folders_to_add   = {L"backup"};
  expected.unchanged_count  = 1;
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, FirstScanReadsNoFile) {
  // A mount with no record: each listed file is a new file, and its import batch reads it.
  FakeDisk disk;
  disk.Add(L"day 2/c.ARW", 3);
  disk.Add(L"a.ARW", 1);
  disk.Add(L"day 1/b.ARW", 2);

  MountScanChangeList expected;
  expected.unread_new_files = {Unread(L"a.ARW"), Unread(L"day 1/b.ARW"), Unread(L"day 2/c.ARW")};
  expected.folders_to_add   = {L"day 1", L"day 2"};
  EXPECT_EQ(Scan(MountScanRecords{}, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, NewPathIsReadOnlyWhenALostRecordHasItsByteSize) {
  // old.ARW is gone. The fingerprint holds the byte size, so only the new path with the size of
  // the lost record can be its file.
  FakeDisk disk;
  disk.Add(L"renamed.ARW", 1);
  disk.Add(L"other size.ARW", 2, kSize + 1);
  const MountScanRecords records{.files = {Present(10, L"old.ARW", 1)}};

  MountScanChangeList    expected;
  expected.moved            = {{10, L"old.ARW", L"renamed.ARW", Values(1)}};
  expected.unread_new_files = {Unread(L"other size.ARW", kSize + 1)};
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{L"renamed.ARW"});
}

TEST(MountScanChangeListTest, StopRequestEndsTheFingerprintReadsWithAnEmptyChangeList) {
  // Five new paths that the lost record can pair with. The stop request arrives in the second
  // read; the comparison reads no third file.
  FakeDisk disk;
  for (const auto* name : {L"a.ARW", L"b.ARW", L"c.ARW", L"d.ARW", L"e.ARW"}) {
    disk.Add(name, 7);
  }
  const MountScanRecords records{.files = {Present(10, L"old.ARW", 1)}};
  std::atomic<bool>      stop{false};
  int                    reads   = 0;
  const auto             read    = disk.Function();

  const auto             changes = BuildMountScanChangeList(
      records, disk.Listing(),
      [&](const MountDirectoryFile& file) {
        if (++reads == 2) stop.store(true);
        return read(file);
      },
      &stop);

  EXPECT_EQ(changes, MountScanChangeList{});
  EXPECT_EQ(disk.Reads(), (Paths{L"a.ARW", L"b.ARW"}));
}

TEST(MountScanChangeListTest, StopRequestEndsTheReadsOfChangedFiles) {
  FakeDisk disk;
  disk.Add(L"a.ARW", 1, kSize, kTime + 1);
  disk.Add(L"b.ARW", 2, kSize, kTime + 1);
  disk.Add(L"c.ARW", 3, kSize, kTime + 1);
  const MountScanRecords records{
      .files = {Present(10, L"a.ARW", 1), Present(11, L"b.ARW", 2), Present(12, L"c.ARW", 3)}};
  std::atomic<bool> stop{false};
  const auto        read    = disk.Function();

  const auto        changes = BuildMountScanChangeList(
      records, disk.Listing(),
      [&](const MountDirectoryFile& file) {
        stop.store(true);
        return read(file);
      },
      &stop);

  EXPECT_EQ(changes, MountScanChangeList{});
  EXPECT_EQ(disk.Reads(), Paths{L"a.ARW"});
}

TEST(MountScanChangeListTest, TwoRecordsWithOneFingerprintMatchBySameNameFirst) {
  // Two copies with equal content. Sorted path order alone would pair a/x with c/y.
  FakeDisk disk;
  disk.Add(L"c/y.ARW", 7);
  disk.Add(L"d/x.ARW", 7);
  const MountScanRecords records{
      .files   = {Present(10, L"a/x.ARW", 7), Present(11, L"b/y.ARW", 7)},
      .folders = {{20, L"a"}, {21, L"b"}, {22, L"c"}, {23, L"d"}},
  };

  MountScanChangeList expected;
  expected.moved             = {{11, L"b/y.ARW", L"c/y.ARW", Values(7)},
                                {10, L"a/x.ARW", L"d/x.ARW", Values(7)}};
  expected.folders_to_remove = {{21, L"b"}, {20, L"a"}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, RecordsWithOneFingerprintAndNoSameNameMatchInSortedPathOrder) {
  // Three copies with equal content appear for two lost records: the records pair with the
  // paths in sorted order, and the third path is a new file.
  FakeDisk disk;
  disk.Add(L"p.ARW", 7);
  disk.Add(L"q.ARW", 7);
  disk.Add(L"r.ARW", 7);
  const MountScanRecords records{.files = {Present(11, L"b.ARW", 7), Present(10, L"a.ARW", 7)}};

  MountScanChangeList    expected;
  expected.moved     = {{10, L"a.ARW", L"p.ARW", Values(7)}, {11, L"b.ARW", L"q.ARW", Values(7)}};
  expected.new_files = {{L"r.ARW", Values(7)}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, CaseOnlyRenameIsAMove) {
  FakeDisk disk;
  disk.Add(L"Trip/img_0001.arw", 1);
  const MountScanRecords records{
      .files   = {Present(10, L"trip/IMG_0001.ARW", 1)},
      .folders = {{20, L"trip"}},
  };

  MountScanChangeList expected;
  expected.moved             = {{10, L"trip/IMG_0001.ARW", L"Trip/img_0001.arw", Values(1)}};
  expected.folders_to_add    = {L"Trip"};
  expected.folders_to_remove = {{20, L"trip"}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, RecordWithNoFileAndNoMatchBecomesMissing) {
  FakeDisk disk;
  disk.Add(L"b.ARW", 2);
  disk.Add(L"other.ARW", 9);
  const MountScanRecords records{
      .files = {Present(11, L"b.ARW", 2), Present(10, L"a.ARW", 1), Missing(12, L"c.ARW", 3)},
  };

  MountScanChangeList expected;
  expected.new_files           = {{L"other.ARW", Values(9)}};
  expected.missing             = {{10, L"a.ARW"}};
  expected.unchanged_count     = 1;
  expected.still_missing_count = 1;
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, MissingRecordWithFileAtItsPathReturns) {
  // a.ARW is back with its recorded values: no read. b.ARW is back with new values: one read.
  FakeDisk disk;
  disk.Add(L"a.ARW", 1);
  disk.Add(L"b.ARW", 8, kSize + 1, kTime);
  const MountScanRecords records{.files = {Missing(10, L"a.ARW", 1), Missing(11, L"b.ARW", 2)}};

  MountScanChangeList    expected;
  expected.returned = {{10, L"a.ARW", Values(1)}, {11, L"b.ARW", Values(8, kSize + 1, kTime)}};
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{L"b.ARW"});
}

TEST(MountScanChangeListTest, MissingRecordWithItsFileAtANewPathIsMoved) {
  FakeDisk disk;
  disk.Add(L"found/a.ARW", 1);
  const MountScanRecords records{.files = {Missing(10, L"a.ARW", 1)}};

  MountScanChangeList    expected;
  expected.moved          = {{10, L"a.ARW", L"found/a.ARW", Values(1)}};
  expected.folders_to_add = {L"found"};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, OrdinaryFileBelowTheRootIsAdopted) {
  // The ordinary photo of a.ARW is adopted even though a lost record has its fingerprint: the
  // path identifies the photo first.
  FakeDisk disk;
  disk.Add(L"a.ARW", 1);
  disk.Add(L"b.ARW", 2);
  const MountScanRecords records{
      .files          = {Present(10, L"old/a.ARW", 1)},
      .folders        = {{20, L"old"}},
      .ordinary_files = {{30, L"a.ARW"}, {31, L"not listed.ARW"}},
  };

  MountScanChangeList expected;
  expected.new_files = {{L"b.ARW", Values(2)}};
  expected.missing   = {{10, L"old/a.ARW"}};
  expected.adopted   = {{30, L"a.ARW", Values(1)}};
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), (Paths{L"a.ARW", L"b.ARW"}));
}

TEST(MountScanChangeListTest, LowestElementIdIsAdoptedWhenTwoOrdinaryFilesHaveOnePath) {
  FakeDisk disk;
  disk.Add(L"a.ARW", 1);
  const MountScanRecords records{
      .ordinary_files = {{42, L"a.ARW"}, {31, L"a.ARW"}, {57, L"a.ARW"}}};

  MountScanChangeList expected;
  expected.adopted = {{31, L"a.ARW", Values(1)}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, ReachableRootWithZeroFilesMakesEachRecordMissing) {
  FakeDisk               disk;
  const MountScanRecords records{
      .files = {Present(10, L"x/a.ARW", 1), Present(11, L"x/y/b.ARW", 2), Present(12, L"c.ARW", 3),
                Missing(13, L"x/d.ARW", 4)},
      .folders = {{20, L"x"}, {21, L"x/y"}, {22, L"empty"}},
  };

  MountScanChangeList expected;
  expected.missing             = {{12, L"c.ARW"}, {10, L"x/a.ARW"}, {11, L"x/y/b.ARW"}};
  expected.folders_to_remove   = {{22, L"empty"}};
  expected.still_missing_count = 1;
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, FolderOfAMissingPhotoStays) {
  // x/y holds a missing photo, so x/y and x stay. z held a photo that moved, so z is removed.
  FakeDisk disk;
  disk.Add(L"w/b.ARW", 2);
  const MountScanRecords records{
      .files   = {Present(10, L"x/y/a.ARW", 1), Present(11, L"z/b.ARW", 2)},
      .folders = {{20, L"x"}, {21, L"x/y"}, {22, L"z"}},
  };

  MountScanChangeList expected;
  expected.moved             = {{11, L"z/b.ARW", L"w/b.ARW", Values(2)}};
  expected.missing           = {{10, L"x/y/a.ARW"}};
  expected.folders_to_add    = {L"w"};
  expected.folders_to_remove = {{22, L"z"}};
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, UnreadableFileIsInNoListAndKeepsItsRecord) {
  // A locked new file, a locked changed file, and a locked copy of a lost record: none of them
  // is in a list, and the lost record becomes missing.
  FakeDisk disk;
  disk.AddLocked(L"new.ARW");
  disk.AddLocked(L"changed.ARW", kSize + 1, kTime);
  disk.AddLocked(L"moved/a.ARW");
  const MountScanRecords records{
      .files = {Present(10, L"changed.ARW", 1), Present(11, L"a.ARW", 2)},
  };

  MountScanChangeList expected;
  expected.missing          = {{11, L"a.ARW"}};
  expected.folders_to_add   = {L"moved"};
  expected.unreadable_count = 3;
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, RecordBelowAnUnreadableDirectoryIsUnverifiedAndMatchesNoNewPath) {
  // locked/ could not be opened. Its record does not become missing, its folder stays, and a new
  // path with the fingerprint of that record waits for a listing that can verify the record.
  FakeDisk disk;
  disk.UnreadableDirectory(L"locked");
  disk.Add(L"open/a copy.ARW", 1);
  disk.Add(L"open/b.ARW", 2);
  const MountScanRecords records{
      .files   = {Present(10, L"locked/a.ARW", 1), Present(11, L"locked/deep/c.ARW", 3)},
      .folders = {{20, L"locked"}, {21, L"locked/deep"}, {22, L"open"}},
  };

  MountScanChangeList expected;
  expected.new_files        = {{L"open/b.ARW", Values(2)}};
  expected.unverified_count = 2;
  expected.deferred_count   = 1;
  EXPECT_EQ(Scan(records, disk), expected);
}

TEST(MountScanChangeListTest, RecordWithAnotherFingerprintVersionMatchesNoNewPath) {
  FakeDisk disk;
  disk.Add(L"b.ARW", 1);
  MountScanFileRecord record        = Present(10, L"a.ARW", 1);
  record.source.fingerprint_version = kSourceFileFingerprintVersion + 1;
  const MountScanRecords records{.files = {record}};

  MountScanChangeList    expected;
  expected.unread_new_files = {Unread(L"b.ARW")};
  expected.missing          = {{10, L"a.ARW"}};
  EXPECT_EQ(Scan(records, disk), expected);
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, IncompleteListingGivesNoChangesAndReadsNoFile) {
  FakeDisk disk;
  disk.Add(L"b.ARW", 2);
  const MountScanRecords records{.files = {Present(10, L"a.ARW", 1)}, .folders = {{20, L"x"}}};

  for (const auto status :
       {MountDirectoryListingStatus::kRootUnreachable, MountDirectoryListingStatus::kStopped}) {
    MountDirectoryListing listing;
    listing.status = status;
    EXPECT_EQ(BuildMountScanChangeList(records, listing, disk.Function()), MountScanChangeList{});
  }
  EXPECT_EQ(disk.Reads(), Paths{});
}

TEST(MountScanChangeListTest, FileMatchesOneRuleOnlyAndListsAreSortedByPath) {
  // One scan with each kind of change. Each element id is in one list, and each list is in
  // sorted path order.
  FakeDisk disk;
  disk.Add(L"z/kept.ARW", 1);
  disk.Add(L"m/changed.ARW", 2, kSize, kTime + 5);
  disk.Add(L"a/renamed.ARW", 3);
  disk.Add(L"b/returned.ARW", 4);
  disk.Add(L"c/adopted.ARW", 5);
  disk.Add(L"c/new 2.ARW", 6);
  disk.Add(L"a/new 1.ARW", 7);
  const MountScanRecords records{
      .files          = {Present(10, L"z/kept.ARW", 1), Present(11, L"m/changed.ARW", 2),
                         Present(12, L"y/old name.ARW", 3), Missing(13, L"b/returned.ARW", 4),
                         Present(14, L"y/gone.ARW", 8)},
      .folders        = {{20, L"z"}, {21, L"m"}, {22, L"y"}, {23, L"b"}},
      .ordinary_files = {{30, L"c/adopted.ARW"}},
  };

  MountScanChangeList expected;
  expected.changed_in_place = {{11, L"m/changed.ARW", Values(2, kSize, kTime + 5)}};
  expected.moved            = {{12, L"y/old name.ARW", L"a/renamed.ARW", Values(3)}};
  expected.new_files        = {{L"a/new 1.ARW", Values(7)}, {L"c/new 2.ARW", Values(6)}};
  expected.missing          = {{14, L"y/gone.ARW"}};
  expected.returned         = {{13, L"b/returned.ARW", Values(4)}};
  expected.adopted          = {{30, L"c/adopted.ARW", Values(5)}};
  expected.folders_to_add   = {L"a", L"c"};
  expected.unchanged_count  = 1;
  EXPECT_EQ(Scan(records, disk), expected);
}

}  // namespace alcedo
