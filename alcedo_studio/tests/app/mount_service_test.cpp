//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF4 (#986) and MF8 (#990): MountService creates, scans, adopts, relocates,
// and removes mounts, and locates and removes missing photos, on real temporary directories and a
// real project database. Each scan of this file runs through ScanAndCheckDirectory, which
// compares the directory content before and after the scan.

#include "app/mount_service.hpp"

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/import_service.hpp"
#include "app/photo_stack_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "support/storage_invariants.hpp"
#include "support/test_temp_directory.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/volume_identity.hpp"

namespace alcedo {
namespace {

namespace stdfs        = std::filesystem;

/// Names, byte sizes, and modification times of every entry below a directory.
using DirectoryContent = std::map<std::wstring, std::pair<std::uintmax_t, stdfs::file_time_type>>;

auto ReadDirectoryContent(const stdfs::path& root) -> DirectoryContent {
  DirectoryContent content;
  std::error_code  ec;
  if (!stdfs::exists(root, ec)) {
    return content;
  }
  for (const auto& entry : stdfs::recursive_directory_iterator(root)) {
    // Read each entry itself. The values that a directory read caches can lag on NTFS: the
    // index of a parent directory updates the time of a changed child directory later.
    const auto& path = entry.path();
    const auto  size = stdfs::is_regular_file(path) ? stdfs::file_size(path) : 0;
    content.emplace(path.lexically_relative(root).generic_wstring(),
                    std::make_pair(size, stdfs::last_write_time(path)));
  }
  return content;
}

/// Encode a 24x16 JPEG whose pixels depend on @p seed, so two seeds give two fingerprints.
void WriteJpeg(const stdfs::path& path, int seed) {
  stdfs::create_directories(path.parent_path());
  constexpr int             kWidth = 24, kHeight = 16, kChannels = 3;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * kChannels);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      auto* px = &pixels[(y * kWidth + x) * kChannels];
      px[0]    = static_cast<std::uint8_t>(x * 10 + seed * 7);
      px[1]    = static_cast<std::uint8_t>(y * 15 + seed * 13);
      px[2]    = static_cast<std::uint8_t>(seed * 31);
    }
  }
  const auto encode_path = path.parent_path() / (path.stem().string() + "_encode.jpg");
  auto       output      = OIIO::ImageOutput::create(encode_path.string());
  ASSERT_TRUE(output);
  const OIIO::ImageSpec spec(kWidth, kHeight, kChannels, OIIO::TypeDesc::UINT8);
  ASSERT_TRUE(output->open(encode_path.string(), spec));
  ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
  ASSERT_TRUE(output->close());
  output.reset();
  stdfs::rename(encode_path, path);
}

class MountServiceTest : public ::testing::Test {
 protected:
  test::TestTempDirectory              temp_dir_;
  stdfs::path                          db_path_;
  stdfs::path                          meta_path_;
  /// The user directories of the test. Only the test changes them.
  stdfs::path                          disk_;
  std::unique_ptr<ProjectService>      project_;
  std::shared_ptr<PipelineMgmtService> pipeline_;
  std::unique_ptr<ImportServiceImpl>   import_;

  void                                 SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
    stdfs::create_directories(temp_dir_ / "disk");
    disk_ = stdfs::canonical(temp_dir_ / "disk");
    OpenProject();
  }

  void TearDown() override {
    CloseProject();
    test::ExpectStoredStorageInvariants(db_path_);
  }

  void OpenProject() {
    project_  = std::make_unique<ProjectService>(db_path_, meta_path_);
    pipeline_ = std::make_shared<PipelineMgmtService>(project_->GetStorage());
    import_   = std::make_unique<ImportServiceImpl>(project_->GetSleeveService(),
                                                    project_->GetImagePoolService(), pipeline_);
  }

  void CloseProject() {
    if (!project_) {
      return;
    }
    import_.reset();
    project_->GetSleeveService()->Sync();
    project_->GetImagePoolService()->SyncWithStorage();
    project_->SaveProject(meta_path_);
    pipeline_.reset();
    project_.reset();
  }

  void ReopenProject() {
    CloseProject();
    OpenProject();
  }

  auto Mounts() -> MountService& { return *project_->GetMountService(); }
  auto Store() -> MountStore& { return project_->GetStorage()->GetMountStore(); }
  auto Sleeve() -> SleeveServiceImpl& { return *project_->GetSleeveService(); }

  /// Scan the mount and check that the scan left the directory content unchanged.
  auto ScanAndCheckDirectory(mount_id_t mount_id, MountScanRequest request = {})
      -> MountScanResult {
    const auto root   = MountOf(mount_id).root_path;
    const auto before = ReadDirectoryContent(root);
    request.mount_id  = mount_id;
    request.importer  = import_.get();
    const auto result = Mounts().ScanMount(request);
    EXPECT_EQ(ReadDirectoryContent(root), before) << "the scan changed the directory";
    return result;
  }

  auto MountOf(mount_id_t mount_id) -> MountRootRecord {
    for (const auto& summary : Mounts().ListMounts()) {
      if (summary.record.id == mount_id) {
        return summary.record;
      }
    }
    throw std::runtime_error("no mount with the id");
  }

  auto SummaryOf(mount_id_t mount_id) -> MountSummary {
    for (const auto& summary : Mounts().ListMounts()) {
      if (summary.record.id == mount_id) {
        return summary;
      }
    }
    throw std::runtime_error("no mount with the id");
  }

  /// The mounted files by relative path (from `Image.image_path`).
  auto FilesOf(mount_id_t mount_id) -> std::map<std::wstring, sl_element_id_t> {
    const auto                              root = MountOf(mount_id).root_path;
    std::map<std::wstring, sl_element_id_t> files;
    for (const auto& location : Store().ReadFileLocationsOfMount(mount_id)) {
      files.emplace(MountRelativePathOf(root, location.image_path).value_or(L"?"),
                    location.element_id);
    }
    return files;
  }

  auto FolderPathsOf(mount_id_t mount_id) -> std::vector<std::wstring> {
    std::vector<std::wstring> paths;
    for (const auto& folder :
         Sleeve().ReadMountTree(MountOf(mount_id).root_folder_id, mount_id).folders) {
      paths.push_back(folder.relative_path);
    }
    return paths;
  }

  /// The relative path of each mounted folder that holds the file; empty text for the root.
  auto HomeFoldersOf(mount_id_t mount_id, sl_element_id_t file_id) -> std::vector<std::wstring> {
    const auto                mount = MountOf(mount_id);
    const auto                tree  = Sleeve().ReadMountTree(mount.root_folder_id, mount_id);
    std::vector<std::wstring> paths;
    const auto                it = tree.home_folders.find(file_id);
    if (it == tree.home_folders.end()) {
      return paths;
    }
    for (const auto folder_id : it->second) {
      if (folder_id == mount.root_folder_id) {
        paths.emplace_back();
      }
      for (const auto& folder : tree.folders) {
        if (folder.folder_id == folder_id) {
          paths.push_back(folder.relative_path);
        }
      }
    }
    return paths;
  }

  auto LocationOf(sl_element_id_t file_id) -> FileLocationRecord {
    for (const auto& location : Store().ReadOrdinaryFileLocations()) {
      if (location.element_id == file_id) {
        return location;
      }
    }
    for (const auto& summary : Mounts().ListMounts()) {
      for (const auto& location : Store().ReadFileLocationsOfMount(summary.record.id)) {
        if (location.element_id == file_id) {
          return location;
        }
      }
    }
    throw std::runtime_error("no file with the id");
  }

  auto ElementOf(sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
    return Sleeve().Read<std::shared_ptr<SleeveElement>>(
        [id](FileSystem& fs) { return fs.Get(id); });
  }

  auto RatingOf(sl_element_id_t file_id) -> int {
    return project_->GetImagePoolService()->Read<int>(
        LocationOf(file_id).image_id,
        [](const std::shared_ptr<Image>& image) { return image->exif_display_.rating_; });
  }

  void SetRating(sl_element_id_t file_id, int rating) {
    // Write_NoSync and a sync: ImagePoolService::Write locks the pool mutex twice (#811).
    auto& pool = *project_->GetImagePoolService();
    pool.Write_NoSync<void>(
        LocationOf(file_id).image_id,
        [rating](const std::shared_ptr<Image>& image) { image->exif_display_.rating_ = rating; });
    ASSERT_TRUE(pool.SyncWithStorage().failed_images_.empty());
  }

  auto RootIdOf(sl_element_id_t file_id) -> root_id_t {
    auto       session = project_->GetStorage()->OpenSession();
    const auto graph   = session.CommitGraph().LoadGraph(file_id);
    EXPECT_TRUE(graph.has_value()) << "the file has no edit history";
    return graph.has_value() ? graph->GetRootId() : root_id_t{};
  }

  auto CreateCollection(const std::wstring& name) -> sl_element_id_t {
    return Sleeve().CreateFolder(L"/", name).first->element_id_;
  }

  auto IsInFolder(sl_element_id_t file_id, sl_element_id_t folder_id) -> bool {
    const auto ids = Sleeve().Read<std::vector<sl_element_id_t>>(
        [folder_id](FileSystem& fs) { return fs.ListFolderContent(folder_id); });
    return std::find(ids.begin(), ids.end(), file_id) != ids.end();
  }

  auto LibraryFileCount() -> std::size_t {
    return project_->GetStorage()->GetElementStore().CountFilesInFolder(0);
  }

  /// A mount of `disk_/<name>` in the library root, scanned once.
  auto MountAndScan(const std::wstring& name) -> mount_id_t {
    const auto mount_id = Mounts().CreateMount(disk_ / name);
    const auto result   = ScanAndCheckDirectory(mount_id);
    EXPECT_EQ(result.status, MountScanStatus::kComplete) << result.error;
    return mount_id;
  }

  /// The tree of the first tests: three levels, a directory with no image, a period directory,
  /// and a file of another type.
  void WriteTrip(const std::wstring& name) {
    const auto root = disk_ / name;
    WriteJpeg(root / "a.jpg", 1);
    WriteJpeg(root / "Day1" / "b.jpg", 2);
    WriteJpeg(root / "Day1" / "Morning" / "c.jpg", 3);
    WriteJpeg(root / "Day2" / "d.jpg", 4);
    stdfs::create_directories(root / "Empty");
    WriteJpeg(root / ".cache" / "e.jpg", 5);
    std::ofstream(root / "notes.txt") << "notes";
  }
};

auto CodeOf(const std::function<void()>& operation) -> std::optional<MountError> {
  try {
    operation();
  } catch (const MountOperationError& error) {
    return error.Code();
  }
  return std::nullopt;
}

TEST_F(MountServiceTest, FirstScanBuildsFoldersAndPhotosOfTheDirectory) {
  WriteTrip(L"Japan");
  const auto mount_id = Mounts().CreateMount(disk_ / "Japan");
  const auto result   = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.imported_count, 4u);
  EXPECT_EQ(result.import_failed_count, 0u);
  EXPECT_EQ(result.folders_added_count, 3u);
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2"}));
  // The directory watcher of MF7 registers the directories of the listing. It holds the
  // directory with no photo too: a file that the user copies there must start a scan.
  auto listed = result.listed_directories;
  std::sort(listed.begin(), listed.end());
  EXPECT_EQ(listed, (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2", L"Empty"}));

  const auto files = FilesOf(mount_id);
  ASSERT_EQ(files.size(), 4u);
  const auto root = MountOf(mount_id).root_path;
  for (const auto& [relative_path, file_id] : files) {
    SCOPED_TRACE(relative_path);
    const auto source = Store().ReadSourceFile(file_id);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->state, SourceFileState::kPresent);
    EXPECT_EQ(source->byte_size, stdfs::file_size(root / stdfs::path(relative_path)));
    EXPECT_EQ(LocationOf(file_id).image_path, root / stdfs::path(relative_path).make_preferred());
    const auto slash = relative_path.rfind(L'/');
    EXPECT_EQ(HomeFoldersOf(mount_id, file_id),
              (std::vector<std::wstring>{
                  slash == std::wstring::npos ? std::wstring{} : relative_path.substr(0, slash)}));
    EXPECT_EQ(ElementOf(file_id)->mount_id_, std::optional<mount_id_t>{mount_id});
    EXPECT_NE(RootIdOf(file_id), root_id_t{});
  }
  EXPECT_TRUE(files.contains(L"Day1/Morning/c.jpg"));
  EXPECT_EQ(LibraryFileCount(), 4u);

  const auto summary = SummaryOf(mount_id);
  EXPECT_EQ(summary.reachability, MountReachability::kOnline);
  EXPECT_EQ(summary.present_count, 4u);
  EXPECT_EQ(summary.missing_count, 0u);
  EXPECT_TRUE(summary.record.last_scan_at.has_value());
}

TEST_F(MountServiceTest, MovedFileKeepsElementIdAndEditHistoryRootId) {
  WriteTrip(L"Japan");
  const auto mount_id   = MountAndScan(L"Japan");
  const auto file_id    = FilesOf(mount_id).at(L"Day1/b.jpg");
  const auto root_id    = RootIdOf(file_id);
  const auto collection = CreateCollection(L"Trips");
  ASSERT_TRUE(Sleeve().LinkFileToFolder(file_id, collection).success_);
  SetRating(file_id, 4);

  stdfs::rename(disk_ / "Japan" / "Day1" / "b.jpg", disk_ / "Japan" / "Day2" / "b moved.jpg");
  std::vector<sl_element_id_t> invalidated;
  MountScanRequest             request;
  request.invalidate_thumbnail = [&invalidated](sl_element_id_t id) { invalidated.push_back(id); };
  const auto result            = ScanAndCheckDirectory(mount_id, request);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.moved_count, 1u);
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(result.missing_count, 0u);
  EXPECT_EQ(invalidated, std::vector<sl_element_id_t>{file_id});
  const auto files = FilesOf(mount_id);
  EXPECT_EQ(files.size(), 4u);
  EXPECT_FALSE(files.contains(L"Day1/b.jpg"));
  ASSERT_TRUE(files.contains(L"Day2/b moved.jpg"));
  EXPECT_EQ(files.at(L"Day2/b moved.jpg"), file_id);
  EXPECT_EQ(RootIdOf(file_id), root_id);
  EXPECT_EQ(RatingOf(file_id), 4);
  EXPECT_EQ(ElementOf(file_id)->element_name_, L"b moved.jpg");
  EXPECT_EQ(HomeFoldersOf(mount_id, file_id), std::vector<std::wstring>{L"Day2"});
  EXPECT_TRUE(IsInFolder(file_id, collection));
  EXPECT_EQ(LibraryFileCount(), 4u);
  EXPECT_EQ(Store().ReadSourceFile(file_id)->state, SourceFileState::kPresent);
}

TEST_F(MountServiceTest, RenamedDirectoryKeepsEachElement) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto before   = FilesOf(mount_id);

  stdfs::rename(disk_ / "Japan" / "Day1", disk_ / "Japan" / "Day One");
  const auto result = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.moved_count, 2u);
  EXPECT_EQ(result.folders_added_count, 2u);
  EXPECT_EQ(result.folders_removed_count, 2u);
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day One", L"Day One/Morning", L"Day2"}));
  const auto after = FilesOf(mount_id);
  EXPECT_EQ(after.at(L"Day One/b.jpg"), before.at(L"Day1/b.jpg"));
  EXPECT_EQ(after.at(L"Day One/Morning/c.jpg"), before.at(L"Day1/Morning/c.jpg"));
  EXPECT_EQ(after.at(L"a.jpg"), before.at(L"a.jpg"));
  EXPECT_EQ(HomeFoldersOf(mount_id, after.at(L"Day One/Morning/c.jpg")),
            std::vector<std::wstring>{L"Day One/Morning"});
  EXPECT_EQ(LibraryFileCount(), 4u);
}

TEST_F(MountServiceTest, DeletedFileBecomesMissingAndKeepsItsRating) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto file_id  = FilesOf(mount_id).at(L"Day2/d.jpg");
  SetRating(file_id, 5);

  stdfs::remove(disk_ / "Japan" / "Day2" / "d.jpg");
  const auto result = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.missing_count, 1u);
  const auto source = Store().ReadSourceFile(file_id);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(source->state, SourceFileState::kMissing);
  EXPECT_TRUE(source->missing_since.has_value());
  EXPECT_EQ(RatingOf(file_id), 5);
  EXPECT_EQ(LocationOf(file_id).image_path, MountOf(mount_id).root_path / "Day2" / "d.jpg");
  // The folder of a missing photo stays, and the photo keeps its home link in it.
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2"}));
  EXPECT_EQ(HomeFoldersOf(mount_id, file_id), std::vector<std::wstring>{L"Day2"});
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 1u);
  EXPECT_EQ(LibraryFileCount(), 4u);

  // A second scan writes nothing for a photo that is still missing.
  const auto again = ScanAndCheckDirectory(mount_id);
  EXPECT_EQ(again.still_missing_count, 1u);
  EXPECT_EQ(again.missing_count, 0u);
  EXPECT_EQ(Store().ReadSourceFile(file_id), source);
}

TEST_F(MountServiceTest, FileThatReturnsBecomesPresent) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto file_id  = FilesOf(mount_id).at(L"Day2/d.jpg");
  const auto aside    = temp_dir_ / "aside.jpg";
  stdfs::rename(disk_ / "Japan" / "Day2" / "d.jpg", aside);
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).missing_count, 1u);

  stdfs::rename(aside, disk_ / "Japan" / "Day2" / "d.jpg");
  std::vector<sl_element_id_t> invalidated;
  MountScanRequest             request;
  request.invalidate_thumbnail = [&invalidated](sl_element_id_t id) { invalidated.push_back(id); };
  const auto result            = ScanAndCheckDirectory(mount_id, request);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.returned_count, 1u);
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(invalidated, std::vector<sl_element_id_t>{file_id});
  const auto source = Store().ReadSourceFile(file_id);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(source->state, SourceFileState::kPresent);
  EXPECT_FALSE(source->missing_since.has_value());
  EXPECT_EQ(FilesOf(mount_id).at(L"Day2/d.jpg"), file_id);
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);
}

TEST_F(MountServiceTest, OrdinaryPhotoBelowTheRootIsAdoptedWithItsCollectionLinks) {
  WriteTrip(L"Japan");
  const auto collection = CreateCollection(L"Trips");
  {
    auto                       job = std::make_shared<ImportJob>();
    std::promise<ImportResult> finished;
    job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
    import_->ImportToFolder({disk_ / "Japan" / "Day1" / "b.jpg"}, L"/Trips", {}, job);
    ASSERT_EQ(finished.get_future().get().imported_, 1u);
    import_->SyncImports(job->import_log_->Snapshot(), L"/Trips");
  }
  const auto ordinary = Store().ReadOrdinaryFileLocations();
  ASSERT_EQ(ordinary.size(), 1u);
  const auto file_id = ordinary.front().element_id;
  const auto root_id = RootIdOf(file_id);
  SetRating(file_id, 3);

  const auto mount_id = Mounts().CreateMount(disk_ / "Japan");
  const auto result   = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.adopted_count, 1u);
  EXPECT_EQ(result.imported_count, 3u);
  EXPECT_EQ(FilesOf(mount_id).at(L"Day1/b.jpg"), file_id);
  EXPECT_EQ(ElementOf(file_id)->mount_id_, std::optional<mount_id_t>{mount_id});
  EXPECT_EQ(HomeFoldersOf(mount_id, file_id), std::vector<std::wstring>{L"Day1"});
  EXPECT_TRUE(IsInFolder(file_id, collection));
  EXPECT_EQ(RootIdOf(file_id), root_id);
  EXPECT_EQ(RatingOf(file_id), 3);
  EXPECT_EQ(Store().ReadSourceFile(file_id)->state, SourceFileState::kPresent);
  // One file has one element (D10).
  EXPECT_TRUE(Store().ReadOrdinaryFileLocations().empty());
  EXPECT_EQ(LibraryFileCount(), 4u);
}

TEST_F(MountServiceTest, UnreachableRootSetsOfflineAndChangesNoRecord) {
  WriteTrip(L"Japan");
  const auto mount_id     = MountAndScan(L"Japan");
  const auto files        = FilesOf(mount_id);
  const auto folders      = FolderPathsOf(mount_id);
  const auto sources      = Store().ReadSourceFilesOfMount(mount_id);
  const auto last_scan_at = MountOf(mount_id).last_scan_at;

  stdfs::rename(disk_ / "Japan", disk_ / "Japan unplugged");
  const auto result =
      Mounts().ScanMount(MountScanRequest{.mount_id = mount_id, .importer = import_.get()});

  EXPECT_EQ(result.status, MountScanStatus::kOffline);
  EXPECT_EQ(result.fingerprint_read_count, 0u);
  EXPECT_TRUE(result.listed_directories.empty());
  EXPECT_EQ(SummaryOf(mount_id).reachability, MountReachability::kOffline);
  EXPECT_EQ(FilesOf(mount_id), files);
  EXPECT_EQ(FolderPathsOf(mount_id), folders);
  EXPECT_EQ(Store().ReadSourceFilesOfMount(mount_id), sources);
  EXPECT_EQ(MountOf(mount_id).last_scan_at, last_scan_at);
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);

  stdfs::rename(disk_ / "Japan unplugged", disk_ / "Japan");
  const auto online = ScanAndCheckDirectory(mount_id);
  EXPECT_EQ(online.status, MountScanStatus::kComplete);
  EXPECT_EQ(online.unchanged_count, 4u);
  EXPECT_EQ(SummaryOf(mount_id).reachability, MountReachability::kOnline);
}

TEST_F(MountServiceTest, NestedDirectoryIsRefusedInBothDirections) {
  WriteTrip(L"Photos");
  const auto mount_id      = Mounts().CreateMount(disk_ / "Photos");
  const auto root_folder   = MountOf(mount_id).root_folder_id;
  const auto library_count = Sleeve().ListFolderEntries(L"/").size();

  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_ / "Photos" / "Day1"); }),
            MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_); }), MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_ / "Photos"); }), MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_ / "Photos" / "Day1" / ".." / "."); }),
            MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_ / "Absent"); }),
            MountError::kDirectoryUnreachable);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(disk_ / "Photos" / "a.jpg"); }),
            MountError::kDirectoryUnreachable);

  EXPECT_EQ(Mounts().ListMounts().size(), 1u);
  EXPECT_EQ(Sleeve().ListFolderEntries(L"/").size(), library_count);
  (void)root_folder;
}

// #1076: the parent of a mount is always the library root, also while a collection exists and
// for a second mount.
TEST_F(MountServiceTest, NewMountIsAChildOfTheLibraryRoot) {
  WriteTrip(L"Japan");
  WriteTrip(L"Italy");
  const auto trip  = CreateCollection(L"Trips");
  const auto japan = MountAndScan(L"Japan");
  const auto italy = MountAndScan(L"Italy");

  for (const auto mount_id : {japan, italy}) {
    const auto folder_id = MountOf(mount_id).root_folder_id;
    EXPECT_TRUE(IsInFolder(folder_id, kLibraryRootFolderId)) << "mount " << mount_id;
    EXPECT_FALSE(IsInFolder(folder_id, trip)) << "mount " << mount_id;
    EXPECT_EQ(ElementOf(folder_id)->mount_id_, mount_id);
  }
}

// #1076: CreateMount stores the volume name, each scan that reaches the volume stores a changed
// name and gives the mount point, and the name stays while the volume is offline.
TEST_F(MountServiceTest, VolumeNameIsStoredAndStaysWhileTheVolumeIsOffline) {
  WriteTrip(L"Card");
  VolumeLabel  label{"EOS_DIGITAL", disk_};
  int          label_reads = 0;
  MountService service(
      project_->GetStorage(), project_->GetSleeveService(), project_->GetImagePoolService(),
      [] { return std::vector<VolumeMountPoint>{}; }, [](const stdfs::path&) { return "card"; },
      [&label, &label_reads](const stdfs::path&) {
        ++label_reads;
        return label;
      });
  const auto mount_id = service.CreateMount(disk_ / "Card");
  EXPECT_EQ(MountOf(mount_id).volume_name, "EOS_DIGITAL");
  const auto summary_of = [&service, mount_id] {
    for (auto& summary : service.ListMounts()) {
      if (summary.record.id == mount_id) {
        return summary;
      }
    }
    throw std::runtime_error("no mount with the id");
  };
  // No scan ran: the mount point is not known yet.
  EXPECT_EQ(summary_of().reachability, MountReachability::kNotScanned);
  EXPECT_TRUE(summary_of().volume_mount_point.empty());

  const auto first = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
  ASSERT_EQ(first.status, MountScanStatus::kComplete);
  EXPECT_FALSE(first.volume_name_updated);
  EXPECT_EQ(summary_of().reachability, MountReachability::kOnline);
  EXPECT_EQ(summary_of().volume_mount_point, disk_);
  EXPECT_EQ(summary_of().record.volume_name, "EOS_DIGITAL");

  // The user renamed the volume. #1088: the result reports the stored name, so the project file
  // is saved.
  label.name           = "Trip Card";
  const auto renamed   = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
  ASSERT_EQ(renamed.status, MountScanStatus::kComplete);
  EXPECT_TRUE(renamed.volume_name_updated);
  EXPECT_EQ(MountOf(mount_id).volume_name, "Trip Card");
  const auto unchanged = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
  EXPECT_FALSE(unchanged.volume_name_updated);

  // The volume is unplugged: the scan does not read the label, and the name stays.
  const int reads_before_offline = label_reads;
  stdfs::rename(disk_ / "Card", disk_ / "Card unplugged");
  label = VolumeLabel{};
  ASSERT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kOffline);
  EXPECT_EQ(label_reads, reads_before_offline);
  EXPECT_EQ(summary_of().reachability, MountReachability::kOffline);
  EXPECT_TRUE(summary_of().volume_mount_point.empty());
  // The stored row (MountStoreTest covers the reopen of the project).
  EXPECT_EQ(MountOf(mount_id).volume_name, "Trip Card");
}

TEST_F(MountServiceTest, SiblingDirectoryWithCommonNamePrefixIsAccepted) {
  WriteJpeg(disk_ / "Photos" / "a.jpg", 1);
  WriteJpeg(disk_ / "Photos2" / "b.jpg", 2);
  const auto first  = MountAndScan(L"Photos");
  const auto second = MountAndScan(L"Photos2");

  EXPECT_NE(first, second);
  EXPECT_EQ(Mounts().ListMounts().size(), 2u);
  EXPECT_EQ(FilesOf(first).size(), 1u);
  EXPECT_EQ(FilesOf(second).size(), 1u);
  EXPECT_TRUE(FilesOf(second).contains(L"b.jpg"));
}

TEST_F(MountServiceTest, CanceledScanIsCompletedByTheNextScan) {
  for (int index = 0; index < 5; ++index) {
    WriteJpeg(disk_ / "Roll" / ("frame_" + std::to_string(index) + ".jpg"), 10 + index);
  }
  const auto        mount_id = Mounts().CreateMount(disk_ / "Roll");
  std::atomic<bool> stop{false};
  MountScanRequest  request;
  request.stop_requested      = &stop;
  request.new_file_batch_size = 2;
  request.on_progress         = [&stop](const MountScanProgress& progress) {
    if (progress.new_files_done >= 2) {
      stop.store(true);
    }
  };
  const auto canceled = ScanAndCheckDirectory(mount_id, request);

  EXPECT_EQ(canceled.status, MountScanStatus::kCanceled);
  EXPECT_EQ(canceled.imported_count, 2u);
  EXPECT_EQ(FilesOf(mount_id).size(), 2u);
  EXPECT_FALSE(MountOf(mount_id).last_scan_at.has_value());

  MountScanRequest next;
  next.new_file_batch_size = 2;
  const auto completed     = ScanAndCheckDirectory(mount_id, next);
  EXPECT_EQ(completed.status, MountScanStatus::kComplete) << completed.error;
  EXPECT_EQ(completed.unchanged_count, 2u);
  EXPECT_EQ(completed.imported_count, 3u);
  EXPECT_EQ(FilesOf(mount_id).size(), 5u);
  EXPECT_TRUE(MountOf(mount_id).last_scan_at.has_value());
}

/// An importer that requests the stop of the scan in its first batch, then imports the batch.
class StopInFirstBatchImporter final : public MountFileImporter {
 public:
  StopInFirstBatchImporter(MountFileImporter& importer, std::atomic<bool>& stop)
      : importer_(importer), stop_(stop) {}

  auto ImportMountedFiles(std::span<const MountFileImportRequest> requests)
      -> std::vector<MountFileImportOutcome> override {
    stop_.store(true);
    return importer_.ImportMountedFiles(requests);
  }

 private:
  MountFileImporter& importer_;
  std::atomic<bool>& stop_;
};

TEST_F(MountServiceTest, StoppedFirstScanReadsOnlyTheFilesOfItsStoredBatch) {
  // #1014: the first scan read the fingerprint of each file before its first import batch. The
  // stop request arrives while batch 2 imports its two files. That batch is stored in full, and
  // the scan reads none of the six other files.
  constexpr int kFileCount = 8;
  for (int index = 0; index < kFileCount; ++index) {
    WriteJpeg(disk_ / "Roll" / ("frame_" + std::to_string(index) + ".jpg"), 20 + index);
  }
  const auto               mount_id = Mounts().CreateMount(disk_ / "Roll");
  std::atomic<bool>        stop{false};
  StopInFirstBatchImporter importer(*import_, stop);
  MountScanRequest         request;
  request.mount_id            = mount_id;
  request.importer            = &importer;
  request.stop_requested      = &stop;
  request.new_file_batch_size = 2;
  std::vector<MountScanProgress> progress;
  request.on_progress = [&progress](const MountScanProgress& step) { progress.push_back(step); };

  const auto stopped  = Mounts().ScanMount(request);

  EXPECT_EQ(stopped.status, MountScanStatus::kCanceled);
  EXPECT_EQ(stopped.fingerprint_read_count, 2u);
  EXPECT_EQ(stopped.imported_count, 2u);
  // Each stored photo has its source record: the batch is complete.
  const auto files = FilesOf(mount_id);
  ASSERT_EQ(files.size(), 2u);
  for (const auto& [path, element_id] : files) {
    const auto source = Store().ReadSourceFile(element_id);
    ASSERT_TRUE(source.has_value()) << "no source record";
    EXPECT_EQ(source->state, SourceFileState::kPresent);
  }
  // The progress shows the total of the directory before the first read.
  ASSERT_EQ(progress.size(), 2u);
  EXPECT_EQ(progress[0].new_files_total, static_cast<std::size_t>(kFileCount));
  EXPECT_EQ(progress[0].new_files_done, 0u);
  EXPECT_EQ(progress[1].new_files_done, 2u);

  const auto completed = ScanAndCheckDirectory(mount_id);
  EXPECT_EQ(completed.status, MountScanStatus::kComplete) << completed.error;
  EXPECT_EQ(completed.unchanged_count, 2u);
  EXPECT_EQ(completed.imported_count, 6u);
  EXPECT_EQ(completed.fingerprint_read_count, 6u);
  EXPECT_EQ(FilesOf(mount_id).size(), static_cast<std::size_t>(kFileCount));
}

/// An importer whose storage write fails for every batch.
class FailingImporter final : public MountFileImporter {
 public:
  auto ImportMountedFiles(std::span<const MountFileImportRequest>)
      -> std::vector<MountFileImportOutcome> override {
    throw std::runtime_error("the Image rows were not written");
  }
};

TEST_F(MountServiceTest, FailedBatchEndsTheScanAndTheNextScanCompletesIt) {
  WriteTrip(L"Japan");
  const auto      mount_id = Mounts().CreateMount(disk_ / "Japan");
  FailingImporter failing;
  const auto      failed =
      Mounts().ScanMount(MountScanRequest{.mount_id = mount_id, .importer = &failing});

  EXPECT_EQ(failed.status, MountScanStatus::kStorageFailure);
  EXPECT_EQ(failed.failed_batch, 2u);
  EXPECT_NE(failed.error.find("the Image rows were not written"), std::string::npos);
  // Batch 1 is stored; the new files have no record, so the next scan finds them again.
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2"}));
  EXPECT_TRUE(FilesOf(mount_id).empty());
  EXPECT_FALSE(MountOf(mount_id).last_scan_at.has_value());

  const auto completed = ScanAndCheckDirectory(mount_id);
  EXPECT_EQ(completed.status, MountScanStatus::kComplete) << completed.error;
  EXPECT_EQ(completed.imported_count, 4u);
  EXPECT_EQ(completed.folders_added_count, 0u);
  EXPECT_EQ(FilesOf(mount_id).size(), 4u);
}

TEST_F(MountServiceTest, FileWithoutASourceRecordGetsOneFromTheNextScan) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto file_id  = FilesOf(mount_id).at(L"Day1/b.jpg");
  const auto stored   = Store().ReadSourceFile(file_id);
  // The state of a batch that stopped after the tree write and before the record write.
  Store().RemoveSourceFiles(std::vector<sl_element_id_t>{file_id});

  const auto result = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.changed_in_place_count, 1u);
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(result.fingerprint_read_count, 1u);
  EXPECT_EQ(Store().ReadSourceFile(file_id), stored);
  EXPECT_EQ(FilesOf(mount_id).at(L"Day1/b.jpg"), file_id);
}

TEST_F(MountServiceTest, UnmountKeepsPhotosAsOrdinaryPhotos) {
  WriteTrip(L"Japan");
  const auto mount_id    = MountAndScan(L"Japan");
  const auto files       = FilesOf(mount_id);
  const auto root_folder = MountOf(mount_id).root_folder_id;
  const auto collection  = CreateCollection(L"Trips");
  const auto linked      = files.at(L"a.jpg");
  ASSERT_TRUE(Sleeve().LinkFileToFolder(linked, collection).success_);
  stdfs::remove(disk_ / "Japan" / "Day2" / "d.jpg");
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).missing_count, 1u);
  std::map<sl_element_id_t, FileLocationRecord> locations;
  for (const auto& [_, file_id] : files) {
    locations.emplace(file_id, LocationOf(file_id));
  }

  Mounts().RemoveMount(mount_id);

  EXPECT_TRUE(Mounts().ListMounts().empty());
  EXPECT_TRUE(Store().ReadFileLocationsOfMount(mount_id).empty());
  EXPECT_EQ(Store().ReadOrdinaryFileLocations().size(), 4u);
  for (const auto& [relative_path, file_id] : files) {
    SCOPED_TRACE(relative_path);
    EXPECT_EQ(ElementOf(file_id)->mount_id_, std::nullopt);
    EXPECT_EQ(LocationOf(file_id), locations.at(file_id));
    EXPECT_TRUE(Store().ReadSourceFile(file_id).has_value());
  }
  EXPECT_EQ(Store().ReadSourceFile(files.at(L"Day2/d.jpg"))->state, SourceFileState::kMissing);
  EXPECT_TRUE(IsInFolder(linked, collection));
  EXPECT_THROW(ElementOf(root_folder), std::exception);
  EXPECT_EQ(LibraryFileCount(), 4u);
  EXPECT_EQ(CodeOf([&] { Mounts().RemoveMount(mount_id); }), MountError::kUnknownMount);

  // The directory can be mounted again; its photos are adopted, not imported a second time.
  const auto again = Mounts().CreateMount(disk_ / "Japan");
  const auto scan  = ScanAndCheckDirectory(again);
  EXPECT_EQ(scan.adopted_count, 3u);
  EXPECT_EQ(scan.imported_count, 0u);
  EXPECT_EQ(LibraryFileCount(), 4u);
}

TEST_F(MountServiceTest, StateIsEqualAfterProjectReopen) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  stdfs::rename(disk_ / "Japan" / "a.jpg", disk_ / "Japan" / "Day2" / "a.jpg");
  stdfs::remove(disk_ / "Japan" / "Day1" / "b.jpg");
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).status, MountScanStatus::kComplete);
  const auto moved_id = FilesOf(mount_id).at(L"Day2/a.jpg");
  const auto root_id  = RootIdOf(moved_id);
  const auto summary  = SummaryOf(mount_id);
  const auto files    = FilesOf(mount_id);
  const auto folders  = FolderPathsOf(mount_id);
  const auto sources  = Store().ReadSourceFilesOfMount(mount_id);
  std::map<sl_element_id_t, std::vector<std::wstring>> homes;
  for (const auto& [_, file_id] : files) {
    homes.emplace(file_id, HomeFoldersOf(mount_id, file_id));
  }

  ReopenProject();

  const auto reopened = SummaryOf(mount_id);
  EXPECT_EQ(reopened.record, summary.record);
  EXPECT_EQ(reopened.present_count, 3u);
  EXPECT_EQ(reopened.missing_count, 1u);
  EXPECT_EQ(reopened.reachability, MountReachability::kNotScanned);
  EXPECT_EQ(FilesOf(mount_id), files);
  EXPECT_EQ(FolderPathsOf(mount_id), folders);
  EXPECT_EQ(Store().ReadSourceFilesOfMount(mount_id), sources);
  for (const auto& [file_id, home] : homes) {
    EXPECT_EQ(HomeFoldersOf(mount_id, file_id), home) << file_id;
  }
  EXPECT_EQ(RootIdOf(moved_id), root_id);

  const auto result = ScanAndCheckDirectory(mount_id);
  EXPECT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.unchanged_count, 3u);
  EXPECT_EQ(result.still_missing_count, 1u);
  EXPECT_EQ(result.fingerprint_read_count, 0u);
  EXPECT_EQ(FilesOf(mount_id), files);
}

TEST_F(MountServiceTest, ScanLeavesTheDirectoryUnchanged) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  // Each case of the scan in one pass: a move, a change in place, a copy, a removal, a new
  // directory. ScanAndCheckDirectory compares the content before and after.
  stdfs::rename(disk_ / "Japan" / "a.jpg", disk_ / "Japan" / "Day1" / "a.jpg");
  WriteJpeg(disk_ / "Japan" / "Day2" / "d.jpg", 40);
  stdfs::copy_file(disk_ / "Japan" / "Day1" / "b.jpg", disk_ / "Japan" / "Day1" / "b copy.jpg");
  stdfs::remove(disk_ / "Japan" / "Day1" / "Morning" / "c.jpg");
  WriteJpeg(disk_ / "Japan" / "Day3" / "f.jpg", 6);

  const auto result = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.moved_count, 1u);
  EXPECT_EQ(result.changed_in_place_count, 1u);
  EXPECT_EQ(result.imported_count, 2u);
  EXPECT_EQ(result.missing_count, 1u);
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2", L"Day3"}));
  EXPECT_EQ(FilesOf(mount_id).size(), 6u);
}

TEST_F(MountServiceTest, SecondScanOfAnUnchangedDirectoryReadsNoFile) {
  constexpr int kFileCount = 2000;
  for (int index = 0; index < kFileCount; ++index) {
    WriteJpeg(disk_ / "Archive" / ("Roll " + std::to_string(index / 100)) /
                  ("frame_" + std::to_string(index) + ".jpg"),
              index);
  }
  const auto mount_id = Mounts().CreateMount(disk_ / "Archive");
  const auto first    = ScanAndCheckDirectory(mount_id);
  ASSERT_EQ(first.status, MountScanStatus::kComplete) << first.error;
  ASSERT_EQ(first.imported_count, static_cast<std::size_t>(kFileCount));
  EXPECT_EQ(first.fingerprint_read_count, static_cast<std::size_t>(kFileCount));

  const auto second = ScanAndCheckDirectory(mount_id);

  EXPECT_EQ(second.status, MountScanStatus::kComplete) << second.error;
  RecordProperty("second_scan_fingerprint_reads", std::to_string(second.fingerprint_read_count));
  EXPECT_EQ(second.fingerprint_read_count, 0u);
  EXPECT_EQ(second.unchanged_count, static_cast<std::size_t>(kFileCount));
  EXPECT_EQ(second.imported_count, 0u);
}

// The time measurement of the MF7 exit criterion: a scan of an unchanged directory of 10 000
// files on a local SSD takes less than 2 seconds in a Release build. The setup imports 10 000
// files, so the test runs only on request:
//   MountServiceTest --gtest_also_run_disabled_tests --gtest_filter=*TenThousand*
TEST_F(MountServiceTest,
       DISABLED_ScanOfAnUnchangedDirectoryOfTenThousandFilesTakesUnderTwoSeconds) {
  constexpr int kFileCount = 10000;
  for (int index = 0; index < kFileCount; ++index) {
    WriteJpeg(disk_ / "Archive" / ("Roll " + std::to_string(index / 100)) /
                  ("frame_" + std::to_string(index) + ".jpg"),
              index);
  }
  const auto mount_id = Mounts().CreateMount(disk_ / "Archive");
  const auto first =
      Mounts().ScanMount(MountScanRequest{.mount_id = mount_id, .importer = import_.get()});
  ASSERT_EQ(first.status, MountScanStatus::kComplete) << first.error;
  ASSERT_EQ(first.imported_count, static_cast<std::size_t>(kFileCount));

  const auto start = std::chrono::steady_clock::now();
  const auto second =
      Mounts().ScanMount(MountScanRequest{.mount_id = mount_id, .importer = import_.get()});
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

  ASSERT_EQ(second.status, MountScanStatus::kComplete) << second.error;
  EXPECT_EQ(second.unchanged_count, static_cast<std::size_t>(kFileCount));
  RecordProperty("unchanged_scan_ms", std::to_string(elapsed_ms));
  std::printf("unchanged scan of %d files: %lld ms\n", kFileCount,
              static_cast<long long>(elapsed_ms));
  EXPECT_LT(elapsed_ms, 2000);
}

// ── MF8 (#990): root relocation, volume search, Locate, and Remove Missing Photos ──────────────

TEST_F(MountServiceTest, RelocatedRootKeepsEachElementId) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto files    = FilesOf(mount_id);
  const auto rated    = files.at(L"Day1/b.jpg");
  const auto root_id  = RootIdOf(rated);
  SetRating(rated, 4);

  stdfs::rename(disk_ / "Japan", disk_ / "Japan 2026");
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).status, MountScanStatus::kOffline);

  Mounts().RelocateRoot(mount_id, disk_ / "Japan 2026");

  EXPECT_EQ(MountOf(mount_id).root_path, stdfs::canonical(disk_ / "Japan 2026"));
  EXPECT_EQ(MountOf(mount_id).volume_identity, ReadVolumeIdentity(disk_ / "Japan 2026"));
  EXPECT_EQ(SummaryOf(mount_id).reachability, MountReachability::kNotScanned);
  EXPECT_EQ(FilesOf(mount_id), files);
  EXPECT_EQ(LocationOf(rated).image_path, MountOf(mount_id).root_path / "Day1" / "b.jpg");

  // The next scan compares the new location: each file is unchanged, none is read again.
  const auto result = ScanAndCheckDirectory(mount_id);
  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.unchanged_count, 4u);
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(result.missing_count, 0u);
  EXPECT_EQ(result.fingerprint_read_count, 0u);
  EXPECT_EQ(FilesOf(mount_id), files);
  EXPECT_EQ(RootIdOf(rated), root_id);
  EXPECT_EQ(RatingOf(rated), 4);
  EXPECT_EQ(LibraryFileCount(), 4u);
}

TEST_F(MountServiceTest, RelocatedRootComparesTheNewLocationInTheNextScan) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto files    = FilesOf(mount_id);

  // The new location holds a part of the old files and one new file.
  stdfs::create_directories(disk_ / "Copy");
  stdfs::copy(disk_ / "Japan", disk_ / "Copy" / "Japan", stdfs::copy_options::recursive);
  stdfs::remove(disk_ / "Copy" / "Japan" / "Day2" / "d.jpg");
  WriteJpeg(disk_ / "Copy" / "Japan" / "Day2" / "f.jpg", 6);

  Mounts().RelocateRoot(mount_id, disk_ / "Copy" / "Japan");
  const auto result = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.missing_count, 1u);
  EXPECT_EQ(result.imported_count, 1u);
  const auto after = FilesOf(mount_id);
  EXPECT_EQ(after.at(L"a.jpg"), files.at(L"a.jpg"));
  EXPECT_EQ(after.at(L"Day1/Morning/c.jpg"), files.at(L"Day1/Morning/c.jpg"));
  EXPECT_EQ(Store().ReadSourceFile(files.at(L"Day2/d.jpg"))->state, SourceFileState::kMissing);
  EXPECT_TRUE(after.contains(L"Day2/f.jpg"));
}

TEST_F(MountServiceTest, RelocateRootRefusesTheDirectoryOfAnotherMount) {
  WriteTrip(L"Japan");
  WriteTrip(L"Italy");
  const auto japan = MountAndScan(L"Japan");
  const auto italy = MountAndScan(L"Italy");
  const auto root  = MountOf(japan).root_path;

  EXPECT_EQ(CodeOf([&] { Mounts().RelocateRoot(japan, disk_ / "Italy" / "Day1"); }),
            MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().RelocateRoot(japan, disk_); }), MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().RelocateRoot(japan, disk_ / "absent"); }),
            MountError::kDirectoryUnreachable);
  EXPECT_EQ(CodeOf([&] { Mounts().RelocateRoot(99, disk_ / "Japan"); }), MountError::kUnknownMount);
  EXPECT_EQ(MountOf(japan).root_path, root);
  EXPECT_EQ(LocationOf(FilesOf(japan).at(L"a.jpg")).image_path, root / "a.jpg");
  (void)italy;
}

TEST_F(MountServiceTest, VolumeWithRecordedIdentityIsFoundOnOpen) {
  // The volume of the test is mounted at `disk_/E` and moves to `disk_/F`, as a card reader
  // that gets a new drive letter.
  WriteTrip(L"E/DCIM");
  std::vector<VolumeMountPoint> volumes;
  MountService                  service(project_->GetStorage(), project_->GetSleeveService(),
                                        project_->GetImagePoolService(), [&volumes] { return volumes; });
  const auto                    mount_id = service.CreateMount(disk_ / "E" / "DCIM");
  const auto                    identity = MountOf(mount_id).volume_identity;
  ASSERT_FALSE(identity.empty()) << "the test volume has no identity";
  ASSERT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kComplete);
  const auto files = FilesOf(mount_id);

  stdfs::rename(disk_ / "E", disk_ / "F");
  volumes            = {VolumeMountPoint{"another volume", disk_ / "E"},
                        VolumeMountPoint{identity, disk_ / "F"}};

  // A scan without the search stays offline.
  const auto offline = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
  EXPECT_EQ(offline.status, MountScanStatus::kOffline);
  EXPECT_FALSE(offline.relocated_root.has_value());

  const auto result = service.ScanMount(
      {.mount_id = mount_id, .importer = import_.get(), .find_moved_volume = true});
  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  ASSERT_TRUE(result.relocated_root.has_value());
  EXPECT_EQ(*result.relocated_root, stdfs::canonical(disk_ / "F" / "DCIM"));
  EXPECT_EQ(MountOf(mount_id).root_path, stdfs::canonical(disk_ / "F" / "DCIM"));
  EXPECT_EQ(result.unchanged_count, 4u);
  EXPECT_EQ(result.missing_count, 0u);
  EXPECT_EQ(FilesOf(mount_id), files);
}

TEST_F(MountServiceTest, VolumeSearchLeavesTheMountOfflineWhenNoVolumeHasTheDirectory) {
  WriteTrip(L"E/DCIM");
  std::vector<VolumeMountPoint> volumes;
  MountService                  service(project_->GetStorage(), project_->GetSleeveService(),
                                        project_->GetImagePoolService(), [&volumes] { return volumes; });
  const auto                    mount_id = service.CreateMount(disk_ / "E" / "DCIM");
  const auto                    identity = MountOf(mount_id).volume_identity;
  ASSERT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kComplete);
  const auto root = MountOf(mount_id).root_path;

  // The volume is found, but the directory is not on it; another volume has the directory.
  stdfs::rename(disk_ / "E" / "DCIM", disk_ / "E" / "Other");
  stdfs::create_directories(disk_ / "G" / "DCIM");
  volumes           = {VolumeMountPoint{identity, disk_ / "E"},
                       VolumeMountPoint{"another volume", disk_ / "G"}};

  const auto result = service.ScanMount(
      {.mount_id = mount_id, .importer = import_.get(), .find_moved_volume = true});
  EXPECT_EQ(result.status, MountScanStatus::kOffline);
  EXPECT_FALSE(result.relocated_root.has_value());
  EXPECT_EQ(MountOf(mount_id).root_path, root);
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);
}

/// The reachability that @p service holds for the mount; each service holds its own.
auto ReachabilityIn(const MountService& service, mount_id_t mount_id) -> MountReachability {
  for (const auto& summary : service.ListMounts()) {
    if (summary.record.id == mount_id) {
      return summary.reachability;
    }
  }
  throw std::runtime_error("no mount with the id");
}

// #1021: the root directory is the mount point of a volume. After the volume is unmounted the
// mount point stays as an empty directory on another volume.
TEST_F(MountServiceTest, RootOnAnotherVolumeSetsOfflineAndChangesNoRecord) {
  WriteTrip(L"Card");
  std::string  root_identity = "card volume";
  MountService service(
      project_->GetStorage(), project_->GetSleeveService(), project_->GetImagePoolService(),
      [] { return std::vector<VolumeMountPoint>{}; },
      [&root_identity](const stdfs::path&) { return root_identity; });
  const auto mount_id = service.CreateMount(disk_ / "Card");
  ASSERT_EQ(MountOf(mount_id).volume_identity, "card volume");
  ASSERT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kComplete);
  const auto files        = FilesOf(mount_id);
  const auto folders      = FolderPathsOf(mount_id);
  const auto sources      = Store().ReadSourceFilesOfMount(mount_id);
  const auto last_scan_at = MountOf(mount_id).last_scan_at;

  // The empty mount point directory of the unmounted card.
  stdfs::rename(disk_ / "Card", disk_ / "Card contents");
  stdfs::create_directories(disk_ / "Card");
  for (const auto& identity : {std::string("system volume"), std::string()}) {
    root_identity     = identity;
    const auto result = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
    EXPECT_EQ(result.status, MountScanStatus::kOffline) << "identity '" << identity << "'";
    EXPECT_EQ(result.fingerprint_read_count, 0u);
    EXPECT_TRUE(result.listed_directories.empty());
    EXPECT_EQ(ReachabilityIn(service, mount_id), MountReachability::kOffline);
    EXPECT_EQ(Store().ReadSourceFilesOfMount(mount_id), sources);
    EXPECT_EQ(FilesOf(mount_id), files);
    EXPECT_EQ(FolderPathsOf(mount_id), folders);
    EXPECT_EQ(MountOf(mount_id).last_scan_at, last_scan_at);
    EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);
  }

  // The card is mounted again at the same mount point.
  stdfs::remove(disk_ / "Card");
  stdfs::rename(disk_ / "Card contents", disk_ / "Card");
  root_identity     = "card volume";
  const auto online = service.ScanMount({.mount_id = mount_id, .importer = import_.get()});
  EXPECT_EQ(online.status, MountScanStatus::kComplete);
  EXPECT_EQ(online.unchanged_count, 4u);
  EXPECT_EQ(online.missing_count, 0u);
  EXPECT_EQ(ReachabilityIn(service, mount_id), MountReachability::kOnline);
}

TEST_F(MountServiceTest, RootOnAnotherVolumeSearchesForTheRecordedVolume) {
  // The card was mounted at `disk_/Card`; it is mounted at `disk_/Card 1` now, and the old mount
  // point stays as an empty directory.
  WriteTrip(L"Card/DCIM");
  std::vector<VolumeMountPoint> volumes;
  stdfs::path                   card_mount_point = disk_ / "Card";
  MountService                  service(
      project_->GetStorage(), project_->GetSleeveService(), project_->GetImagePoolService(),
      [&volumes] { return volumes; },
      [&card_mount_point](const stdfs::path& directory) {
        return IsPathAtOrBelow(directory, card_mount_point) ? std::string("card volume")
                                                                             : std::string("system volume");
      });
  const auto mount_id = service.CreateMount(disk_ / "Card" / "DCIM");
  ASSERT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kComplete);
  const auto files = FilesOf(mount_id);

  stdfs::rename(disk_ / "Card", disk_ / "Card 1");
  stdfs::create_directories(disk_ / "Card" / "DCIM");
  card_mount_point = disk_ / "Card 1";
  volumes          = {VolumeMountPoint{"system volume", disk_},
                      VolumeMountPoint{"card volume", card_mount_point}};

  // A scan without the search stays offline at the empty mount point.
  EXPECT_EQ(service.ScanMount({.mount_id = mount_id, .importer = import_.get()}).status,
            MountScanStatus::kOffline);
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);

  const auto result = service.ScanMount(
      {.mount_id = mount_id, .importer = import_.get(), .find_moved_volume = true});
  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  ASSERT_TRUE(result.relocated_root.has_value());
  EXPECT_EQ(*result.relocated_root, stdfs::canonical(disk_ / "Card 1" / "DCIM"));
  EXPECT_EQ(result.unchanged_count, 4u);
  EXPECT_EQ(result.missing_count, 0u);
  EXPECT_EQ(FilesOf(mount_id), files);
}

TEST_F(MountServiceTest, LocatedMissingPhotoKeepsEditHistory) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto file_id  = FilesOf(mount_id).at(L"Day1/b.jpg");
  const auto root_id  = RootIdOf(file_id);
  SetRating(file_id, 4);
  const auto collection = CreateCollection(L"Selects");
  ASSERT_TRUE(Sleeve()
                  .Write<void>([&](FileSystem& fs) { fs.LinkFileToFolder(file_id, collection); })
                  .success_);

  // Another application rewrote the file and saved it in a new directory: the fingerprint is
  // different, so the scan reports the photo missing.
  stdfs::remove(disk_ / "Japan" / "Day1" / "b.jpg");
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).missing_count, 1u);
  const auto root = MountOf(mount_id).root_path;
  WriteJpeg(root / "Edited" / "Final" / "b-final.jpg", 9);

  Mounts().LocateMissingFile(file_id, root / "Edited" / "Final" / "b-final.jpg");

  const auto source = Store().ReadSourceFile(file_id);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(source->state, SourceFileState::kPresent);
  EXPECT_FALSE(source->missing_since.has_value());
  EXPECT_EQ(source->byte_size, stdfs::file_size(root / "Edited" / "Final" / "b-final.jpg"));
  EXPECT_EQ(FilesOf(mount_id).at(L"Edited/Final/b-final.jpg"), file_id);
  EXPECT_EQ(LocationOf(file_id).image_path, root / "Edited" / "Final" / "b-final.jpg");
  EXPECT_EQ(HomeFoldersOf(mount_id, file_id), std::vector<std::wstring>{L"Edited/Final"});
  EXPECT_EQ(RootIdOf(file_id), root_id);
  EXPECT_EQ(RatingOf(file_id), 4);
  EXPECT_TRUE(IsInFolder(file_id, collection));
  EXPECT_EQ(SummaryOf(mount_id).missing_count, 0u);

  // The next scan finds the located file unchanged and imports nothing.
  const auto result = ScanAndCheckDirectory(mount_id);
  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.imported_count, 0u);
  EXPECT_EQ(result.unchanged_count, 4u);
  EXPECT_EQ(result.folders_added_count, 0u);
  EXPECT_EQ(LibraryFileCount(), 4u);
}

TEST_F(MountServiceTest, FileOutsideTheMountIsRefusedForLocate) {
  WriteTrip(L"Japan");
  const auto mount_id = MountAndScan(L"Japan");
  const auto files    = FilesOf(mount_id);
  const auto file_id  = files.at(L"Day2/d.jpg");
  stdfs::remove(disk_ / "Japan" / "Day2" / "d.jpg");
  ASSERT_EQ(ScanAndCheckDirectory(mount_id).missing_count, 1u);
  const auto root = MountOf(mount_id).root_path;
  WriteJpeg(disk_ / "Elsewhere" / "d.jpg", 4);
  const auto locate = [&](sl_element_id_t id, const stdfs::path& file) {
    return CodeOf([&] { Mounts().LocateMissingFile(id, file); });
  };

  EXPECT_EQ(locate(file_id, disk_ / "Elsewhere" / "d.jpg"), MountError::kFileOutsideMount);
  EXPECT_EQ(locate(file_id, root / ".cache" / "e.jpg"), MountError::kFileIsNotListed);
  EXPECT_EQ(locate(file_id, root / "notes.txt"), MountError::kFileIsNotListed);
  EXPECT_EQ(locate(file_id, root / "a.jpg"), MountError::kFileHasPhoto);
  EXPECT_EQ(locate(file_id, root / "absent.jpg"), MountError::kFileUnreadable);
  // Locate takes a missing photo only.
  EXPECT_EQ(locate(files.at(L"a.jpg"), root / "Day1" / "b.jpg"), MountError::kPhotoIsNotMissing);

  EXPECT_EQ(Store().ReadSourceFile(file_id)->state, SourceFileState::kMissing);
  EXPECT_EQ(LocationOf(file_id).image_path, root / "Day2" / "d.jpg");
  EXPECT_EQ(HomeFoldersOf(mount_id, file_id), std::vector<std::wstring>{L"Day2"});
}

TEST_F(MountServiceTest, RemoveMissingPhotosRepairsStacksAndKeepsOtherPhotos) {
  WriteTrip(L"Japan");
  WriteTrip(L"Italy");
  const auto japan       = MountAndScan(L"Japan");
  const auto italy       = MountAndScan(L"Italy");
  const auto files       = FilesOf(japan);
  const auto italy_files = FilesOf(italy);
  const auto stacks      = project_->GetPhotoStackService();
  const std::array members{files.at(L"a.jpg"), files.at(L"Day1/b.jpg"),
                           files.at(L"Day2/d.jpg"), italy_files.at(L"a.jpg")};
  const auto stack = stacks->CreateStack(members, members.front());
  ASSERT_TRUE(stack.Changed());
  stdfs::remove(disk_ / "Japan" / "a.jpg");
  stdfs::remove(disk_ / "Japan" / "Day2" / "d.jpg");
  stdfs::remove(disk_ / "Italy" / "a.jpg");
  ASSERT_EQ(ScanAndCheckDirectory(japan).missing_count, 2u);
  ASSERT_EQ(ScanAndCheckDirectory(italy).missing_count, 1u);
  const auto                   before = ReadDirectoryContent(disk_);

  std::vector<sl_element_id_t> edit_data_removed;
  const auto                   removal =
      Mounts().RemoveMissingPhotos(japan, [&](std::span<const sl_element_id_t> ids) {
        edit_data_removed.assign(ids.begin(), ids.end());
        pipeline_->DeletePipelines(ids);
      });

  auto removed = removal.element_ids;
  std::sort(removed.begin(), removed.end());
  auto expected = std::vector<sl_element_id_t>{files.at(L"a.jpg"), files.at(L"Day2/d.jpg")};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(removed, expected);
  EXPECT_EQ(removal.image_ids.size(), 2u);
  std::sort(edit_data_removed.begin(), edit_data_removed.end());
  EXPECT_EQ(edit_data_removed, expected);

  for (const auto id : expected) {
    EXPECT_FALSE(Store().ReadSourceFile(id).has_value());
    auto session = project_->GetStorage()->OpenSession();
    EXPECT_FALSE(session.CommitGraph().LoadGraph(id).has_value());
  }
  EXPECT_EQ(stacks->ReadMemberIds(stack.stack_id),
            (std::vector<sl_element_id_t>{members[1], members[3]}));
  test::ExpectStorageInvariants(*project_->GetStorage());
  EXPECT_EQ(FilesOf(japan), (std::map<std::wstring, sl_element_id_t>{
                                {L"Day1/b.jpg", files.at(L"Day1/b.jpg")},
                                {L"Day1/Morning/c.jpg", files.at(L"Day1/Morning/c.jpg")}}));
  EXPECT_EQ(SummaryOf(japan).missing_count, 0u);
  EXPECT_EQ(SummaryOf(japan).present_count, 2u);
  // The missing photo of the other mount stays.
  EXPECT_EQ(FilesOf(italy), italy_files);
  EXPECT_EQ(SummaryOf(italy).missing_count, 1u);
  EXPECT_EQ(LibraryFileCount(), 6u);
  EXPECT_EQ(ReadDirectoryContent(disk_), before);

  // Nothing is missing anymore.
  EXPECT_TRUE(Mounts().RemoveMissingPhotos(japan, {}).element_ids.empty());
}

}  // namespace
}  // namespace alcedo
