//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF4 (#986): MountService creates, scans, adopts, and removes mounts on
// real temporary directories and a real project database. Each scan of this file runs through
// ScanAndCheckDirectory, which compares the directory content before and after the scan.

#include "app/mount_service.hpp"

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "support/storage_invariants.hpp"
#include "support/test_temp_directory.hpp"
#include "type/mount_records.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/file/mount_directory_listing.hpp"

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
    const auto mount_id = Mounts().CreateMount(0, disk_ / name);
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
  const auto mount_id = Mounts().CreateMount(0, disk_ / "Japan");
  const auto result   = ScanAndCheckDirectory(mount_id);

  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  EXPECT_EQ(result.imported_count, 4u);
  EXPECT_EQ(result.import_failed_count, 0u);
  EXPECT_EQ(result.folders_added_count, 3u);
  EXPECT_EQ(FolderPathsOf(mount_id),
            (std::vector<std::wstring>{L"Day1", L"Day1/Morning", L"Day2"}));

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

  const auto mount_id = Mounts().CreateMount(0, disk_ / "Japan");
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
  const auto mount_id      = Mounts().CreateMount(0, disk_ / "Photos");
  const auto root_folder   = MountOf(mount_id).root_folder_id;
  const auto library_count = Sleeve().ListFolderEntries(L"/").size();

  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_ / "Photos" / "Day1"); }),
            MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_); }), MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_ / "Photos"); }), MountError::kNestedMount);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_ / "Photos" / "Day1" / ".." / "."); }),
            MountError::kNestedMount);
  stdfs::create_directories(disk_ / "Other");
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(root_folder, disk_ / "Other"); }),
            MountError::kParentIsMounted);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_ / "Absent"); }),
            MountError::kDirectoryUnreachable);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(0, disk_ / "Photos" / "a.jpg"); }),
            MountError::kDirectoryUnreachable);
  EXPECT_EQ(CodeOf([&] { Mounts().CreateMount(9999, disk_ / "Other"); }),
            MountError::kParentIsNotAFolder);

  EXPECT_EQ(Mounts().ListMounts().size(), 1u);
  EXPECT_EQ(Sleeve().ListFolderEntries(L"/").size(), library_count);
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
  const auto        mount_id = Mounts().CreateMount(0, disk_ / "Roll");
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
  const auto      mount_id = Mounts().CreateMount(0, disk_ / "Japan");
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
  const auto again = Mounts().CreateMount(0, disk_ / "Japan");
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
  const auto mount_id = Mounts().CreateMount(0, disk_ / "Archive");
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

}  // namespace
}  // namespace alcedo
