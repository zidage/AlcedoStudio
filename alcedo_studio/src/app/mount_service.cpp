//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mount_service.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/mount_scan_change_list.hpp"
#include "app/mount_tree.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/source_file_fingerprint.hpp"
#include "utils/file/volume_identity.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

namespace stdfs = std::filesystem;

auto NowUtc() -> std::time_t { return std::chrono::system_clock::to_time_t(TimeProvider::Now()); }

/// The parent directory of a relative path; empty for a file of the mount root.
auto ParentDirectory(const std::wstring& relative_path) -> std::wstring {
  const auto separator = relative_path.rfind(L'/');
  return separator == std::wstring::npos ? std::wstring{} : relative_path.substr(0, separator);
}

auto LastName(const std::wstring& relative_path) -> std::wstring {
  const auto separator = relative_path.rfind(L'/');
  return separator == std::wstring::npos ? relative_path : relative_path.substr(separator + 1);
}

/// The root directory plus a relative path, with the separator of the platform.
auto PathBelowRoot(const stdfs::path& root, const std::wstring& relative_path) -> stdfs::path {
  stdfs::path path  = root;
  std::size_t begin = 0;
  while (true) {
    const auto separator = relative_path.find(L'/', begin);
    path /= relative_path.substr(
        begin, separator == std::wstring::npos ? std::wstring::npos : separator - begin);
    if (separator == std::wstring::npos) {
      return path;
    }
    begin = separator + 1;
  }
}

auto PresentRecord(sl_element_id_t element_id, const MountScanSourceValues& values)
    -> SourceFileRecord {
  SourceFileRecord record;
  record.element_id          = element_id;
  record.byte_size           = values.byte_size;
  record.modified_ns         = values.modified_ns;
  record.fingerprint         = values.fingerprint.hash;
  record.fingerprint_version = values.fingerprint.version;
  record.state               = SourceFileState::kPresent;
  return record;
}

void RequireSynced(const SyncResult& sync) {
  if (!sync.success_) {
    throw std::runtime_error("MountService: the sleeve tree was not written: " + sync.message_);
  }
}

/// `Image.image_path` and the file name of each image of @p locations below @p root, in one pool
/// sync.
void WriteImageLocations(ImagePoolService& image_pool_service, const stdfs::path& root,
                         const std::vector<std::pair<image_id_t, std::wstring>>& locations) {
  if (locations.empty()) {
    return;
  }
  for (const auto& [image_id, relative_path] : locations) {
    auto path = PathBelowRoot(root, relative_path);
    auto name = LastName(relative_path);
    image_pool_service.Write_NoSync<void>(image_id,
                                          [&path, &name](const std::shared_ptr<Image>& image) {
                                            image->image_path_ = path;
                                            image->image_name_ = name;
                                          });
  }
  const auto status = image_pool_service.SyncWithStorage();
  if (!status.failed_images_.empty()) {
    const auto& failure = status.failed_images_.front();
    throw std::runtime_error(std::format("MountService: the Image row {} was not written: {}",
                                         failure.image_id_, failure.message_));
  }
}

/// One stored mounted file of the scan: its image and its source record.
struct ScannedFile {
  image_id_t       image_id = 0;
  /// The stored record, or a record with zero values when a batch stopped before the record was
  /// written. A zero record matches no listed file, so the file is changed in place or missing.
  SourceFileRecord source;
};

/**
 * @brief One run of ScanMount for one mount: the record read, the comparison, and the batches.
 *
 * The run lives inside ScanMount and holds the point-in-time inputs of the scan. It changes the
 * tree only through SleeveServiceImpl, `Image.image_path` only through ImagePoolService, and the
 * source records only through MountStore.
 */
class MountScanRun {
 public:
  MountScanRun(Storage& storage, SleeveServiceImpl& sleeve_service,
               ImagePoolService& image_pool_service, const MountRootRecord& mount,
               const MountScanRequest& request, MountScanResult& result)
      : mounts_(storage.GetMountStore()),
        sleeve_service_(sleeve_service),
        image_pool_service_(image_pool_service),
        mount_(mount),
        request_(request),
        result_(result) {}

  void Execute(const MountDirectoryListing& listing) {
    const auto records = ReadRecords();
    const auto changes = BuildMountScanChangeList(
        records, listing, [this](const MountDirectoryFile& file) { return ReadFingerprint(file); },
        request_.stop_requested);
    // A stop request in the comparison gives an empty change list. No batch has started.
    if (StopRequested()) {
      result_.status = MountScanStatus::kCanceled;
      return;
    }
    result_.unchanged_count     = changes.unchanged_count;
    result_.still_missing_count = changes.still_missing_count;
    result_.unreadable_count    = changes.unreadable_count;
    result_.unverified_count    = changes.unverified_count;
    result_.deferred_count      = changes.deferred_count;

    batch_                      = 1;
    ApplyFirstBatch(changes);
    ReportProgress(changes.new_files.size() + changes.unread_new_files.size(), 0);
    if (!ImportNewFiles(changes)) {
      result_.status = MountScanStatus::kCanceled;
      return;
    }
    mounts_.UpdateMountLastScanTime(mount_.id, NowUtc());
    result_.status = MountScanStatus::kComplete;
  }

  [[nodiscard]] auto Batch() const -> std::size_t { return batch_; }

 private:
  /// The stored state of the mount in the form of the comparison (Mounted Folder Plan, MF3).
  auto ReadRecords() -> MountScanRecords {
    MountScanRecords records;
    // The folders only: a memory read that loads no file element (Sleeve Node Cache Plan, SN5).
    for (const auto& folder : sleeve_service_.ReadMountFolders(mount_.root_folder_id, mount_.id)) {
      records.folders.push_back(MountScanFolderRecord{folder.folder_id, folder.relative_path});
    }

    std::unordered_map<sl_element_id_t, SourceFileRecord> sources;
    for (auto& source : mounts_.ReadSourceFilesOfMount(mount_.id)) {
      sources.emplace(source.element_id, std::move(source));
    }
    for (const auto& location : mounts_.ReadFileLocationsOfMount(mount_.id)) {
      // `Image.image_path` is the location of the file (P2). A path that is not below the root
      // (a stored state that the scan did not write) gets a key that no listed path has, so the
      // file is moved or missing.
      auto relative_path = MountRelativePathOf(mount_.root_path, location.image_path)
                               .value_or(L"/" + location.image_path.generic_wstring());
      SourceFileRecord source;
      source.element_id = location.element_id;
      if (const auto it = sources.find(location.element_id); it != sources.end()) {
        source = it->second;
      }
      files_.emplace(location.element_id, ScannedFile{location.image_id, source});
      records.files.push_back(
          MountScanFileRecord{location.element_id, std::move(relative_path), std::move(source)});
    }

    for (const auto& location : mounts_.ReadOrdinaryFileLocations()) {
      if (auto relative_path = MountRelativePathOf(mount_.root_path, location.image_path)) {
        ordinary_images_.emplace(location.element_id, location.image_id);
        records.ordinary_files.push_back(
            MountScanOrdinaryFile{location.element_id, std::move(*relative_path)});
      }
    }
    return records;
  }

  /// Folder changes, moved, adopted, changed, returned, and missing files: the tree, then
  /// `Image.image_path`, then the source records.
  void ApplyFirstBatch(const MountScanChangeList& changes) {
    MountTreeChanges tree_changes;
    tree_changes.mount_id       = mount_.id;
    tree_changes.root_folder_id = mount_.root_folder_id;
    tree_changes.folders_to_add = changes.folders_to_add;
    for (const auto& folder : changes.folders_to_remove) {
      tree_changes.folders_to_remove.push_back(folder.relative_path);
    }
    const auto place = [&tree_changes](sl_element_id_t file_id, const std::wstring& path) {
      tree_changes.placements.push_back(
          MountTreeFilePlacement{file_id, ParentDirectory(path), LastName(path)});
    };
    for (const auto& move : changes.moved) {
      place(move.element_id, move.relative_path);
    }
    for (const auto& adoption : changes.adopted) {
      place(adoption.element_id, adoption.relative_path);
    }
    for (const auto& returned : changes.returned) {
      place(returned.element_id, returned.relative_path);
    }
    if (!tree_changes.folders_to_add.empty() || !tree_changes.placements.empty() ||
        !tree_changes.folders_to_remove.empty()) {
      RequireSynced(sleeve_service_.ApplyMountTreeChanges(tree_changes));
    }

    std::vector<std::pair<image_id_t, std::wstring>> locations;
    for (const auto& move : changes.moved) {
      locations.emplace_back(files_.at(move.element_id).image_id, move.relative_path);
    }
    for (const auto& adoption : changes.adopted) {
      locations.emplace_back(ordinary_images_.at(adoption.element_id), adoption.relative_path);
    }
    WriteImageLocations(image_pool_service_, mount_.root_path, locations);

    std::vector<SourceFileRecord> sources;
    for (const auto& update : changes.changed_in_place) {
      sources.push_back(PresentRecord(update.element_id, update.values));
    }
    for (const auto& update : changes.returned) {
      sources.push_back(PresentRecord(update.element_id, update.values));
    }
    for (const auto& update : changes.adopted) {
      sources.push_back(PresentRecord(update.element_id, update.values));
    }
    for (const auto& move : changes.moved) {
      sources.push_back(PresentRecord(move.element_id, move.values));
    }
    const auto missing_since = NowUtc();
    for (const auto& missing : changes.missing) {
      auto record          = files_.at(missing.element_id).source;
      record.state         = SourceFileState::kMissing;
      record.missing_since = missing_since;
      sources.push_back(std::move(record));
    }
    mounts_.UpsertSourceFiles(sources);

    result_.moved_count            = changes.moved.size();
    result_.adopted_count          = changes.adopted.size();
    result_.changed_in_place_count = changes.changed_in_place.size();
    result_.returned_count         = changes.returned.size();
    result_.missing_count          = changes.missing.size();
    result_.folders_added_count    = changes.folders_to_add.size();
    result_.folders_removed_count  = changes.folders_to_remove.size();

    if (request_.invalidate_thumbnail) {
      for (const auto& update : changes.changed_in_place) {
        request_.invalidate_thumbnail(update.element_id);
      }
      for (const auto& move : changes.moved) {
        request_.invalidate_thumbnail(move.element_id);
      }
      for (const auto& update : changes.returned) {
        request_.invalidate_thumbnail(update.element_id);
      }
    }
  }

  [[nodiscard]] auto StopRequested() const -> bool {
    return request_.stop_requested != nullptr && request_.stop_requested->load();
  }

  auto ReadFingerprint(const MountDirectoryFile& file) -> SourceFileFingerprintResult {
    ++result_.fingerprint_read_count;
    return ComputeSourceFileFingerprint(PathBelowRoot(mount_.root_path, file.relative_path));
  }

  /**
   * @brief Import the new files in batches: first the files that the comparison read, then the
   *        files of MountScanChangeList::unread_new_files.
   *
   * A batch reads the fingerprint of each of its unread files, then imports its files, then
   * writes their source records. The stop request is read before each batch and before each
   * fingerprint read, which is before the first write of the batch. A batch that started its
   * import completes.
   *
   * @return false when the stop request ended the scan.
   */
  auto ImportNewFiles(const MountScanChangeList& changes) -> bool {
    const auto compared_count = changes.new_files.size();
    const auto new_file_count = compared_count + changes.unread_new_files.size();
    if (new_file_count == 0) {
      return true;
    }
    std::unordered_map<std::wstring, sl_element_id_t> folder_at{{L"", mount_.root_folder_id}};
    for (const auto& folder : sleeve_service_.ReadMountFolders(mount_.root_folder_id, mount_.id)) {
      folder_at.emplace(folder.relative_path, folder.folder_id);
    }

    const auto batch_size = std::max<std::size_t>(1, request_.new_file_batch_size);
    for (std::size_t begin = 0; begin < new_file_count; begin += batch_size) {
      if (StopRequested()) {
        return false;
      }
      ++batch_;
      const auto                          end = std::min(new_file_count, begin + batch_size);
      std::vector<MountFileImportRequest> requests;
      std::vector<MountScanSourceValues>  values;
      for (std::size_t index = begin; index < end; ++index) {
        const bool  compared = index < compared_count;
        const auto& relative_path =
            compared ? changes.new_files[index].relative_path
                     : changes.unread_new_files[index - compared_count].relative_path;
        const auto folder = folder_at.find(ParentDirectory(relative_path));
        if (folder == folder_at.end()) {
          ++result_.import_failed_count;
          continue;
        }
        if (compared) {
          values.push_back(changes.new_files[index].values);
        } else {
          if (StopRequested()) {
            return false;
          }
          const auto& file        = changes.unread_new_files[index - compared_count];
          const auto  fingerprint = ReadFingerprint(file);
          if (!fingerprint.HasFingerprint()) {
            // The file stays without a record; the next scan reads it again.
            ++result_.unreadable_count;
            continue;
          }
          values.push_back(
              MountScanSourceValues{file.byte_size, file.modified_ns, fingerprint.fingerprint});
        }
        requests.push_back(MountFileImportRequest{PathBelowRoot(mount_.root_path, relative_path),
                                                  folder->second, mount_.id});
      }

      const auto                    outcomes = request_.importer->ImportMountedFiles(requests);
      std::vector<SourceFileRecord> sources;
      for (std::size_t index = 0; index < outcomes.size(); ++index) {
        if (outcomes[index].element_id.has_value()) {
          sources.push_back(PresentRecord(*outcomes[index].element_id, values[index]));
        } else {
          ++result_.import_failed_count;
        }
      }
      mounts_.UpsertSourceFiles(sources);
      result_.imported_count += sources.size();
      ReportProgress(new_file_count, end);
    }
    return true;
  }

  void ReportProgress(std::size_t total, std::size_t done) const {
    if (request_.on_progress) {
      request_.on_progress(MountScanProgress{total, done});
    }
  }

  MountStore&                                      mounts_;
  SleeveServiceImpl&                               sleeve_service_;
  ImagePoolService&                                image_pool_service_;
  const MountRootRecord&                           mount_;
  const MountScanRequest&                          request_;
  MountScanResult&                                 result_;
  std::size_t                                      batch_ = 0;
  std::unordered_map<sl_element_id_t, ScannedFile> files_;
  std::unordered_map<sl_element_id_t, image_id_t>  ordinary_images_;
};

auto MountRootName(const stdfs::path& root) -> file_name_t {
  if (auto name = root.filename().wstring(); !name.empty()) {
    return name;
  }
  if (auto name = root.root_name().wstring(); !name.empty()) {
    return name;
  }
  return root.wstring();
}

}  // namespace

MountService::MountService(std::shared_ptr<Storage>           storage,
                           std::shared_ptr<SleeveServiceImpl> sleeve_service,
                           std::shared_ptr<ImagePoolService>  image_pool_service,
                           VolumeMountPointList               list_volumes,
                           VolumeIdentityReader               read_volume_identity,
                           VolumeLabelReader                  read_volume_label)
    : storage_(std::move(storage)),
      sleeve_service_(std::move(sleeve_service)),
      image_pool_service_(std::move(image_pool_service)),
      list_volumes_(list_volumes ? std::move(list_volumes)
                                 : VolumeMountPointList(&ListVolumeMountPoints)),
      read_volume_identity_(read_volume_identity ? std::move(read_volume_identity)
                                                 : VolumeIdentityReader(&ReadVolumeIdentity)),
      read_volume_label_(read_volume_label ? std::move(read_volume_label)
                                           : VolumeLabelReader(&ReadVolumeLabel)) {
  if (!storage_ || !sleeve_service_ || !image_pool_service_) {
    throw std::invalid_argument("MountService: storage, sleeve, and image pool are required");
  }
}

auto MountService::CreateMount(const std::filesystem::path& directory) -> mount_id_t {
  std::lock_guard lock(operation_mutex_);

  std::error_code ec;
  const auto      root = stdfs::canonical(directory, ec);
  if (ec || !stdfs::is_directory(root, ec) || ec) {
    throw MountOperationError(MountError::kDirectoryUnreachable,
                              std::format("MountService: '{}' is not a reachable directory",
                                          conv::ToBytes(directory.wstring())));
  }

  auto& mounts = storage_->GetMountStore();
  for (const auto& mount : mounts.ReadMountRoots()) {
    if (IsPathAtOrBelow(root, mount.root_path) || IsPathAtOrBelow(mount.root_path, root)) {
      throw MountOperationError(
          MountError::kNestedMount,
          std::format("MountService: '{}' is inside or contains the mounted directory '{}'",
                      conv::ToBytes(root.wstring()), conv::ToBytes(mount.root_path.wstring())));
    }
  }

  MountRootRecord record;
  record.id              = mounts.ReadNextMountId();
  record.volume_identity = read_volume_identity_(root);
  record.root_path       = root;
  auto label             = read_volume_label_(root);
  record.volume_name     = std::move(label.name);

  auto [folder, sync]    = sleeve_service_->CreateMountRootFolder(MountRootName(root), record.id);
  record.root_folder_id  = folder->element_id_;
  std::string error      = sync.message_;
  if (sync.success_) {
    try {
      mounts.InsertMountRoot(record);
      // The volume is reached, but no scan ran: the mount stays kNotScanned until its first
      // scan, which records the mount point again.
      SetReachability(record.id, MountReachability::kNotScanned);
      return record.id;
    } catch (const std::exception& e) {
      error = e.what();
    }
  }
  // Take the folder back, so a failed call leaves no mounted folder without a mount.
  sleeve_service_->RemoveMountTree(record.root_folder_id, record.id, {});
  throw MountOperationError(MountError::kStorageFailure,
                            "MountService: the mount was not stored: " + error);
}

auto MountService::ScanMount(const MountScanRequest& request) -> MountScanResult {
  if (request.importer == nullptr) {
    throw std::invalid_argument("MountService: a scan needs an importer");
  }
  std::lock_guard lock(operation_mutex_);
  auto            mount = FindMount(request.mount_id);

  MountScanResult result;
  // The root path of an unmounted volume can stay as an empty directory on another volume
  // (#1021): that root is unreachable too, so no photo of the mount becomes missing.
  auto            listing = ListRootOnRecordedVolume(mount, request.stop_requested);
  if (listing.status == MountDirectoryListingStatus::kRootUnreachable &&
      request.find_moved_volume) {
    if (const auto moved_root = FindRootOnMovedVolume(mount)) {
      try {
        mount.root_path       = RelocateRootLocked(mount, *moved_root);
        result.relocated_root = mount.root_path;
        listing               = ListRootOnRecordedVolume(mount, request.stop_requested);
      } catch (const MountOperationError& e) {
        // The found directory is refused (another mount holds it) or the write failed: the mount
        // stays offline at its recorded root.
        if (e.Code() == MountError::kStorageFailure) {
          result.status = MountScanStatus::kStorageFailure;
          result.error  = e.what();
          return result;
        }
      }
    }
  }
  if (listing.status == MountDirectoryListingStatus::kRootUnreachable) {
    SetReachability(mount.id, MountReachability::kOffline);
    result.status = MountScanStatus::kOffline;
    return result;
  }
  try {
    result.volume_name_updated = RecordVolumeReached(mount);
  } catch (const std::exception& e) {
    result.status = MountScanStatus::kStorageFailure;
    result.error  = e.what();
    return result;
  }
  if (listing.status == MountDirectoryListingStatus::kStopped) {
    result.status = MountScanStatus::kCanceled;
    return result;
  }
  result.listed_directories = listing.directories;

  MountScanRun run(*storage_, *sleeve_service_, *image_pool_service_, mount, request, result);
  try {
    run.Execute(listing);
  } catch (const std::exception& e) {
    result.status       = MountScanStatus::kStorageFailure;
    result.failed_batch = run.Batch();
    result.error        = e.what();
  }
  return result;
}

void MountService::RemoveMount(mount_id_t mount_id) {
  std::lock_guard lock(operation_mutex_);
  const auto      mount  = FindMount(mount_id);
  auto&           mounts = storage_->GetMountStore();
  try {
    std::vector<sl_element_id_t> file_ids;
    for (const auto& location : mounts.ReadFileLocationsOfMount(mount_id)) {
      file_ids.push_back(location.element_id);
    }
    const auto sync = sleeve_service_->RemoveMountTree(mount.root_folder_id, mount_id, file_ids);
    if (!sync.success_) {
      throw std::runtime_error(sync.message_);
    }
    mounts.RemoveMountRoot(mount_id);
  } catch (const std::exception& e) {
    throw MountOperationError(
        MountError::kStorageFailure,
        std::format("MountService: the mount {} was not removed: {}", mount_id, e.what()));
  }
  std::lock_guard session_lock(session_mutex_);
  session_states_.erase(mount_id);
}

void MountService::RelocateRoot(mount_id_t mount_id, const std::filesystem::path& directory) {
  std::lock_guard lock(operation_mutex_);
  RelocateRootLocked(FindMount(mount_id), directory);
}

auto MountService::RelocateRootLocked(const MountRootRecord&       mount,
                                      const std::filesystem::path& directory) -> stdfs::path {
  std::error_code ec;
  const auto      root = stdfs::canonical(directory, ec);
  if (ec || !stdfs::is_directory(root, ec) || ec) {
    throw MountOperationError(MountError::kDirectoryUnreachable,
                              std::format("MountService: '{}' is not a reachable directory",
                                          conv::ToBytes(directory.wstring())));
  }
  auto& mounts = storage_->GetMountStore();
  for (const auto& other : mounts.ReadMountRoots()) {
    if (other.id != mount.id &&
        (IsPathAtOrBelow(root, other.root_path) || IsPathAtOrBelow(other.root_path, root))) {
      throw MountOperationError(
          MountError::kNestedMount,
          std::format("MountService: '{}' is inside or contains the mounted directory '{}'",
                      conv::ToBytes(root.wstring()), conv::ToBytes(other.root_path.wstring())));
    }
  }

  try {
    // The relative paths come from the old root; a path that is not below it is left to the
    // next scan, which finds the file by its fingerprint.
    std::vector<std::pair<image_id_t, std::wstring>> locations;
    for (const auto& location : mounts.ReadFileLocationsOfMount(mount.id)) {
      if (auto relative_path = MountRelativePathOf(mount.root_path, location.image_path)) {
        locations.emplace_back(location.image_id, std::move(*relative_path));
      }
    }
    mounts.UpdateMountRootLocation(mount.id, root, read_volume_identity_(root));
    WriteImageLocations(*image_pool_service_, root, locations);
  } catch (const std::exception& e) {
    throw MountOperationError(
        MountError::kStorageFailure,
        std::format("MountService: the mount {} was not relocated: {}", mount.id, e.what()));
  }
  SetReachability(mount.id, MountReachability::kNotScanned);
  return root;
}

auto MountService::FindRootOnMovedVolume(const MountRootRecord& mount) const
    -> std::optional<stdfs::path> {
  if (mount.volume_identity.empty()) {
    return std::nullopt;
  }
  std::vector<VolumeMountPoint> volumes;
  try {
    volumes = list_volumes_();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  // The old mount point of the volume is a start of the root path, so the path below it is an
  // end of the elements after the root name. The longest end is tried first. The mount point
  // itself is a candidate only for a root that was the root of its volume.
  std::vector<stdfs::path> elements;
  for (const auto& element : mount.root_path.relative_path()) {
    if (!element.empty()) {
      elements.push_back(element);
    }
  }
  for (const auto& volume : volumes) {
    if (volume.identity != mount.volume_identity) {
      continue;
    }
    for (std::size_t first = 0; first <= elements.size(); ++first) {
      if (first == elements.size() && !elements.empty()) {
        break;
      }
      stdfs::path candidate = volume.mount_point;
      for (std::size_t index = first; index < elements.size(); ++index) {
        candidate /= elements[index];
      }
      std::error_code ec;
      if (stdfs::is_directory(candidate, ec) && !ec &&
          read_volume_identity_(candidate) == mount.volume_identity) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

auto MountService::ListRootOnRecordedVolume(const MountRootRecord&   mount,
                                            const std::atomic<bool>* stop_requested) const
    -> MountDirectoryListing {
  // A mount without a recorded identity (a volume without one) has no check. A root whose volume
  // has no readable identity now is not on the recorded volume either: the empty mount point of
  // an unmounted volume can lie on a file system without an identity (tmpfs on Linux).
  if (!mount.volume_identity.empty() &&
      read_volume_identity_(mount.root_path) != mount.volume_identity) {
    MountDirectoryListing listing;
    listing.status = MountDirectoryListingStatus::kRootUnreachable;
    return listing;
  }
  return ListMountDirectory(mount.root_path, stop_requested);
}

void MountService::LocateMissingFile(sl_element_id_t              element_id,
                                     const std::filesystem::path& file) {
  std::lock_guard                lock(operation_mutex_);
  auto&                          mounts = storage_->GetMountStore();

  std::shared_ptr<SleeveElement> element;
  try {
    element = sleeve_service_->Read<std::shared_ptr<SleeveElement>>(
        [element_id](FileSystem& fs) { return fs.Get(element_id); });
  } catch (const std::exception&) {
  }
  const auto source = mounts.ReadSourceFile(element_id);
  if (!element || element->type_ != ElementType::FILE || !element->IsLive() ||
      !element->mount_id_.has_value() || !source.has_value() ||
      source->state != SourceFileState::kMissing) {
    throw MountOperationError(
        MountError::kPhotoIsNotMissing,
        std::format("MountService: the element {} is not a missing mounted photo", element_id));
  }
  const auto      mount = FindMount(*element->mount_id_);

  std::error_code ec;
  const auto      path = stdfs::canonical(file, ec);
  if (ec) {
    throw MountOperationError(
        MountError::kFileUnreadable,
        std::format("MountService: '{}' cannot be read", conv::ToBytes(file.wstring())));
  }
  if (!MountRelativePathOf(mount.root_path, path).has_value()) {
    throw MountOperationError(
        MountError::kFileOutsideMount,
        std::format("MountService: '{}' is not below the mounted directory '{}'",
                    conv::ToBytes(path.wstring()), conv::ToBytes(mount.root_path.wstring())));
  }
  const auto listed = ReadMountDirectoryFile(mount.root_path, path);
  if (!listed.has_value()) {
    throw MountOperationError(
        MountError::kFileIsNotListed,
        std::format("MountService: a scan does not list '{}'", conv::ToBytes(path.wstring())));
  }

  std::unordered_map<sl_element_id_t, SourceFileState> states;
  for (const auto& record : mounts.ReadSourceFilesOfMount(mount.id)) {
    states.emplace(record.element_id, record.state);
  }
  image_id_t image_id = 0;
  for (const auto& location : mounts.ReadFileLocationsOfMount(mount.id)) {
    if (location.element_id == element_id) {
      image_id = location.image_id;
      continue;
    }
    const auto state = states.find(location.element_id);
    if (state != states.end() && state->second == SourceFileState::kPresent &&
        MountRelativePathOf(mount.root_path, location.image_path) == listed->relative_path) {
      throw MountOperationError(MountError::kFileHasPhoto,
                                std::format("MountService: the photo {} has the file '{}'",
                                            location.element_id, conv::ToBytes(path.wstring())));
    }
  }

  const auto fingerprint = ComputeSourceFileFingerprint(path);
  if (!fingerprint.HasFingerprint() || image_id == 0) {
    throw MountOperationError(
        MountError::kFileUnreadable,
        std::format("MountService: '{}' cannot be read", conv::ToBytes(path.wstring())));
  }

  try {
    MountTreeChanges changes;
    changes.mount_id       = mount.id;
    changes.root_folder_id = mount.root_folder_id;
    std::unordered_set<std::wstring> folders;
    for (const auto& folder : sleeve_service_->ReadMountFolders(mount.root_folder_id, mount.id)) {
      folders.insert(folder.relative_path);
    }
    // A directory that the last scan did not see gets its folders now, a parent first.
    const auto directory = ParentDirectory(listed->relative_path);
    for (std::size_t next = 0; !directory.empty();) {
      const auto end    = directory.find(L'/', next);
      const auto prefix = directory.substr(0, end);
      if (!folders.contains(prefix)) {
        changes.folders_to_add.push_back(prefix);
      }
      if (end == std::wstring::npos) {
        break;
      }
      next = end + 1;
    }
    changes.placements.push_back(
        MountTreeFilePlacement{element_id, directory, LastName(listed->relative_path)});
    RequireSynced(sleeve_service_->ApplyMountTreeChanges(changes));

    WriteImageLocations(*image_pool_service_, mount.root_path, {{image_id, listed->relative_path}});

    const std::vector<SourceFileRecord> records{PresentRecord(
        element_id,
        MountScanSourceValues{listed->byte_size, listed->modified_ns, fingerprint.fingerprint})};
    mounts.UpsertSourceFiles(records);
  } catch (const std::exception& e) {
    throw MountOperationError(
        MountError::kStorageFailure,
        std::format("MountService: the photo {} was not located: {}", element_id, e.what()));
  }
}

auto MountService::RemoveMissingPhotos(mount_id_t                  mount_id,
                                       const MountEditDataRemoval& remove_edit_data)
    -> MountMissingPhotoRemoval {
  std::lock_guard lock(operation_mutex_);
  (void)FindMount(mount_id);
  auto&                        mounts = storage_->GetMountStore();

  std::vector<sl_element_id_t> missing_ids;
  for (const auto& record : mounts.ReadSourceFilesOfMount(mount_id)) {
    if (record.state == SourceFileState::kMissing) {
      missing_ids.push_back(record.element_id);
    }
  }
  MountMissingPhotoRemoval removal;
  if (missing_ids.empty()) {
    return removal;
  }

  try {
    auto [files, sync] = sleeve_service_->DeleteMissingMountedFiles(mount_id, missing_ids);
    if (!sync.success_) {
      throw std::runtime_error(sync.message_);
    }
    for (const auto& file : files) {
      removal.element_ids.push_back(file->element_id_);
      removal.image_ids.push_back(file->image_id_);
    }
  } catch (const std::exception& e) {
    throw MountOperationError(
        MountError::kStorageFailure,
        std::format("MountService: the missing photos of the mount {} were not removed: {}",
                    mount_id, e.what()));
  }

  if (remove_edit_data) {
    remove_edit_data(removal.element_ids);
  }
  std::vector<image_id_t> image_ids;
  std::copy_if(removal.image_ids.begin(), removal.image_ids.end(), std::back_inserter(image_ids),
               [](image_id_t image_id) { return image_id != 0; });
  image_pool_service_->RemoveBatch(image_ids);
  const auto status = image_pool_service_->SyncWithStorage();
  if (!status.failed_images_.empty()) {
    const auto& failure = status.failed_images_.front();
    throw MountOperationError(
        MountError::kStorageFailure,
        std::format("MountService: the photos are removed, but the Image row {} was not: {}",
                    failure.image_id_, failure.message_));
  }
  return removal;
}

auto MountService::ListMounts() const -> std::vector<MountSummary> {
  auto&                     mounts = storage_->GetMountStore();
  std::vector<MountSummary> summaries;
  for (auto& record : mounts.ReadMountRoots()) {
    MountSummary summary;
    for (const auto& source : mounts.ReadSourceFilesOfMount(record.id)) {
      (source.state == SourceFileState::kMissing ? summary.missing_element_ids
                                                 : summary.present_element_ids)
          .push_back(source.element_id);
    }
    summary.present_count = summary.present_element_ids.size();
    summary.missing_count = summary.missing_element_ids.size();
    {
      std::lock_guard lock(session_mutex_);
      if (const auto it = session_states_.find(record.id); it != session_states_.end()) {
        summary.reachability       = it->second.reachability;
        summary.volume_mount_point = it->second.volume_mount_point;
      }
    }
    summary.record = std::move(record);
    summaries.push_back(std::move(summary));
  }
  return summaries;
}

auto MountService::FindMount(mount_id_t mount_id) const -> MountRootRecord {
  for (auto& mount : storage_->GetMountStore().ReadMountRoots()) {
    if (mount.id == mount_id) {
      return mount;
    }
  }
  throw MountOperationError(MountError::kUnknownMount,
                            std::format("MountService: no mount has the id {}", mount_id));
}

void MountService::SetReachability(mount_id_t mount_id, MountReachability reachability) {
  std::lock_guard lock(session_mutex_);
  session_states_[mount_id] = MountSessionState{reachability, {}};
}

void MountService::SetOnline(mount_id_t mount_id, std::filesystem::path mount_point) {
  std::lock_guard lock(session_mutex_);
  session_states_[mount_id] = MountSessionState{MountReachability::kOnline, std::move(mount_point)};
}

auto MountService::RecordVolumeReached(const MountRootRecord& mount) -> bool {
  auto label = read_volume_label_(mount.root_path);
  SetOnline(mount.id, std::move(label.mount_point));
  // An unchanged name is not written again, so a scan that finds no change writes nothing.
  if (label.name == mount.volume_name) {
    return false;
  }
  storage_->GetMountStore().UpdateMountVolumeName(mount.id, label.name);
  return true;
}

}  // namespace alcedo
