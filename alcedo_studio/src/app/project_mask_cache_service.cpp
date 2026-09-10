//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/project_mask_cache_service.hpp"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "edit/mask/mask_asset.hpp"
#include "utils/string/convert.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace alcedo {
namespace {

constexpr std::array<char, 8> kMagic = {'A', 'L', 'C', 'R', '8', 'C', 'C', 'H'};
constexpr std::uint32_t       kPackedR8FormatId = 1;
std::atomic<std::uint64_t>    g_temp_sequence{0};

auto ProcessIdText() -> std::string {
#ifdef _WIN32
  return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}

void AppendU32Le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendU64Le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  AppendU32Le(out, static_cast<std::uint32_t>(value));
  AppendU32Le(out, static_cast<std::uint32_t>(value >> 32));
}

void AppendF32BitsLe(std::vector<std::uint8_t>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32Le(out, bits);
}

void AppendBytes(std::vector<std::uint8_t>& out, std::string_view text) {
  AppendU32Le(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

auto ReadU32Le(std::span<const std::uint8_t> bytes, std::size_t* offset) -> std::uint32_t {
  if (*offset + 4 > bytes.size()) {
    throw std::runtime_error("Project Mask cache file is truncated");
  }
  const auto value = static_cast<std::uint32_t>(bytes[*offset]) |
                     (static_cast<std::uint32_t>(bytes[*offset + 1]) << 8) |
                     (static_cast<std::uint32_t>(bytes[*offset + 2]) << 16) |
                     (static_cast<std::uint32_t>(bytes[*offset + 3]) << 24);
  *offset += 4;
  return value;
}

auto ReadU64Le(std::span<const std::uint8_t> bytes, std::size_t* offset) -> std::uint64_t {
  const auto lo = ReadU32Le(bytes, offset);
  const auto hi = ReadU32Le(bytes, offset);
  return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

auto ReadF32Le(std::span<const std::uint8_t> bytes, std::size_t* offset) -> float {
  const auto bits = ReadU32Le(bytes, offset);
  float      value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

auto ReadCountedString(std::span<const std::uint8_t> bytes, std::size_t* offset) -> std::string {
  const auto size = ReadU32Le(bytes, offset);
  if (size > 4096 || *offset + size > bytes.size()) {
    throw std::runtime_error("Project Mask cache string field is invalid");
  }
  std::string text(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return text;
}

auto HexEncode(std::string_view text) -> std::string {
  constexpr char digits[] = "0123456789abcdef";
  std::string    encoded;
  encoded.reserve(text.size() * 2);
  for (const unsigned char byte : text) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
}

auto NormalizeChosenRoot(std::filesystem::path root) -> std::filesystem::path {
  if (root.empty()) {
    throw std::invalid_argument("Project Mask cache root must not be empty");
  }
  std::error_code error;
  auto            absolute = std::filesystem::absolute(root, error);
  if (error) {
    throw std::invalid_argument("Project Mask cache root is not a usable path");
  }
  return absolute.lexically_normal();
}

auto PathText(const std::filesystem::path& path) -> std::string {
  return conv::ToBytes(path.wstring());
}

auto AssignError(std::string* error, std::string message) -> bool {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

auto FlushWrittenFile(std::ofstream& stream, const std::filesystem::path& path, std::string* error)
    -> bool {
  stream.flush();
  if (!stream) {
    return AssignError(error, "Project Mask cache temporary write did not complete");
  }
  stream.close();
#ifdef _WIN32
  const HANDLE handle = ::CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
  }
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
  return true;
}

auto AtomicReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                       std::string* error) -> bool {
#ifdef _WIN32
  if (::ReplaceFileW(destination.wstring().c_str(), source.wstring().c_str(), nullptr,
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
    return true;
  }
  if (::GetLastError() == ERROR_FILE_NOT_FOUND) {
    if (::MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
  }
  return AssignError(error, "Project Mask cache atomic replace failed (Win32 error " +
                                std::to_string(::GetLastError()) + ")");
#else
  std::error_code ec;
  std::filesystem::rename(source, destination, ec);
  if (ec) {
    return AssignError(error, "Project Mask cache atomic replace failed: " + ec.message());
  }
  return true;
#endif
}

auto EncodeRecord(std::string_view project_uuid, const ProjectMaskCacheRecord& record)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(128 + project_uuid.size() + record.identity.node_id.Value().size() +
                record.recipe_fingerprint.size() + record.producer_id.size() + record.pixels.size());
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32Le(bytes, kProjectMaskCacheFormatVersion);
  AppendU32Le(bytes, record.raster_algorithm_version);
  AppendU32Le(bytes, kPackedR8FormatId);
  AppendBytes(bytes, project_uuid);
  AppendU32Le(bytes, record.identity.image_id);
  AppendBytes(bytes, record.identity.node_id.Value());
  AppendU32Le(bytes, record.output_extent.width);
  AppendU32Le(bytes, record.output_extent.height);
  AppendF32BitsLe(bytes, record.reference_bounds.x);
  AppendF32BitsLe(bytes, record.reference_bounds.y);
  AppendF32BitsLe(bytes, record.reference_bounds.w);
  AppendF32BitsLe(bytes, record.reference_bounds.h);
  AppendU32Le(bytes, record.source_grid.width);
  AppendU32Le(bytes, record.source_grid.height);
  AppendBytes(bytes, record.recipe_fingerprint);
  AppendBytes(bytes, record.producer_id);
  AppendU64Le(bytes, static_cast<std::uint64_t>(record.pixels.size()));
  bytes.insert(bytes.end(), record.pixels.begin(), record.pixels.end());
  const auto checksum = XXH3_64bits(bytes.data(), bytes.size());
  AppendU64Le(bytes, checksum);
  return bytes;
}

auto DecodeRecord(std::span<const std::uint8_t> bytes, std::string_view expected_uuid)
    -> ProjectMaskCacheRecord {
  if (bytes.size() < kMagic.size() + 16) {
    throw std::runtime_error("Project Mask cache file is truncated");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    throw std::runtime_error("Project Mask cache magic does not match");
  }
  const auto checksum_offset = bytes.size() - 8;
  const auto stored_checksum = [&] {
    std::size_t offset = checksum_offset;
    return ReadU64Le(bytes, &offset);
  }();
  const auto computed = XXH3_64bits(bytes.data(), checksum_offset);
  if (computed != stored_checksum) {
    throw std::runtime_error("Project Mask cache checksum mismatch");
  }
  std::size_t offset = kMagic.size();
  const auto  format = ReadU32Le(bytes, &offset);
  if (format != kProjectMaskCacheFormatVersion) {
    throw std::runtime_error("Project Mask cache format is unsupported");
  }
  ProjectMaskCacheRecord record;
  record.raster_algorithm_version = ReadU32Le(bytes, &offset);
  const auto packed_format        = ReadU32Le(bytes, &offset);
  if (packed_format != kPackedR8FormatId) {
    throw std::runtime_error("Project Mask cache pixel format is unsupported");
  }
  const auto uuid = ReadCountedString(bytes, &offset);
  if (uuid != expected_uuid) {
    throw std::runtime_error("Project Mask cache file belongs to another project");
  }
  record.identity.image_id = ReadU32Le(bytes, &offset);
  record.identity.node_id  = NodeId{ReadCountedString(bytes, &offset)};
  record.output_extent.width  = ReadU32Le(bytes, &offset);
  record.output_extent.height = ReadU32Le(bytes, &offset);
  record.reference_bounds.x   = ReadF32Le(bytes, &offset);
  record.reference_bounds.y   = ReadF32Le(bytes, &offset);
  record.reference_bounds.w   = ReadF32Le(bytes, &offset);
  record.reference_bounds.h   = ReadF32Le(bytes, &offset);
  record.source_grid.width    = ReadU32Le(bytes, &offset);
  record.source_grid.height   = ReadU32Le(bytes, &offset);
  record.recipe_fingerprint   = ReadCountedString(bytes, &offset);
  record.producer_id          = ReadCountedString(bytes, &offset);
  const auto pixel_bytes      = ReadU64Le(bytes, &offset);
  if (offset + pixel_bytes != checksum_offset) {
    throw std::runtime_error("Project Mask cache pixel payload size is invalid");
  }
  record.pixels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(checksum_offset));
  ValidateProjectMaskCacheRecord(record);
  return record;
}

auto ReadEntireFile(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("Project Mask cache file does not exist");
  }
  const auto size = static_cast<std::size_t>(stream.tellg());
  stream.seekg(0);
  std::vector<std::uint8_t> bytes(size);
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (!stream) {
    throw std::runtime_error("Project Mask cache file is incomplete");
  }
  return bytes;
}

auto IsOwnedCacheFileName(const std::filesystem::path& path) -> bool {
  return path.extension() == kProjectMaskCacheFileExtension;
}

auto IsOwnedTempFileName(std::string_view name) -> bool {
  return name.find(".r8cache.tmp.") != std::string_view::npos;
}

auto RemovePath(const std::filesystem::path& path, std::string* error) -> bool {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return AssignError(error, "Failed to remove " + PathText(path) + ": " + ec.message());
  }
  return true;
}

}  // namespace

void ValidateProjectMaskCacheRecord(const ProjectMaskCacheRecord& record) {
  if (record.identity.node_id.Empty()) {
    throw std::invalid_argument("Project Mask cache NodeId must not be empty");
  }
  if (record.recipe_fingerprint.empty()) {
    throw std::invalid_argument("Project Mask cache recipe fingerprint must not be empty");
  }
  if (record.producer_id.empty()) {
    throw std::invalid_argument("Project Mask cache producer id must not be empty");
  }
  if (record.raster_algorithm_version != kBrushRasterAlgorithmVersion) {
    throw std::invalid_argument("Project Mask cache raster algorithm version is unsupported");
  }
  if (record.source_grid.Empty() || record.source_grid.width > kMaximumRasterMaskAxis ||
      record.source_grid.height > kMaximumRasterMaskAxis) {
    throw std::invalid_argument("Project Mask cache source grid axes must be in [1, 4096]");
  }
  MaskAssetDescriptor descriptor;
  descriptor.extent           = record.output_extent;
  descriptor.reference_bounds = record.reference_bounds;
  ValidateMaskAssetPixels(descriptor, record.pixels);
}

ProjectMaskCacheReader::~ProjectMaskCacheReader() {
  if (release_) {
    release_();
  }
}

ProjectMaskCacheReader::ProjectMaskCacheReader(ProjectMaskCacheReader&& other) noexcept
    : record_(std::move(other.record_)), release_(std::move(other.release_)) {
  other.release_ = nullptr;
}

auto ProjectMaskCacheReader::operator=(ProjectMaskCacheReader&& other) noexcept
    -> ProjectMaskCacheReader& {
  if (this == &other) {
    return *this;
  }
  if (release_) {
    release_();
  }
  record_        = std::move(other.record_);
  release_       = std::move(other.release_);
  other.release_ = nullptr;
  return *this;
}

auto ProjectMaskCacheReader::Get() const -> const ProjectMaskCacheRecord& {
  if (!record_) {
    throw std::runtime_error("Project Mask cache reader is empty");
  }
  return *record_;
}

ProjectMaskCacheService::ProjectMaskCacheService(std::string project_uuid,
                                                 std::filesystem::path chosen_root)
    : project_uuid_(std::move(project_uuid)), chosen_root_(NormalizeChosenRoot(std::move(chosen_root))) {
  (void)ProjectMaskCacheNamespaceDirectory(chosen_root_, project_uuid_);
  writer_ = std::thread([this] { WriterLoop(); });
}

ProjectMaskCacheService::~ProjectMaskCacheService() { ShutdownWriter(); }

auto ProjectMaskCacheService::ProjectUuid() const -> std::string {
  std::lock_guard lock(mutex_);
  return project_uuid_;
}

auto ProjectMaskCacheService::ChosenRoot() const -> std::filesystem::path {
  std::lock_guard lock(mutex_);
  return chosen_root_;
}

auto ProjectMaskCacheService::StorageGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return generation_;
}

auto ProjectMaskCacheService::PrepareNamespace(const std::filesystem::path& chosen_root,
                                               std::string_view project_uuid, std::string* error)
    -> bool {
  try {
    const auto normalized = NormalizeChosenRoot(chosen_root);
    const auto ns         = ProjectMaskCacheNamespaceDirectory(normalized, project_uuid);
    std::error_code ec;
    if (std::filesystem::exists(normalized, ec) && !std::filesystem::is_directory(normalized, ec)) {
      return AssignError(error, "Project Mask cache root exists and is not a directory");
    }
    std::filesystem::create_directories(ns, ec);
    if (ec) {
      return AssignError(error, "Failed to create project Mask cache namespace: " + ec.message());
    }
    if (std::filesystem::is_symlink(ns)) {
      return AssignError(error, "Project Mask cache namespace must not be a symbolic link");
    }
    return true;
  } catch (const std::exception& exception) {
    return AssignError(error, exception.what());
  }
}

auto ProjectMaskCacheService::SlotKeyFrom(const ProjectMaskCacheSlotIdentity& identity) const
    -> SlotKey {
  return SlotKey{identity.image_id, std::string(identity.node_id.Value())};
}

auto ProjectMaskCacheService::SlotPath(const std::filesystem::path& chosen_root,
                                       const ProjectMaskCacheSlotIdentity& identity) const
    -> std::filesystem::path {
  const auto ns = ProjectMaskCacheNamespaceDirectory(chosen_root, project_uuid_);
  return ns / std::to_string(identity.image_id) /
         (HexEncode(identity.node_id.Value()) + std::string(kProjectMaskCacheFileExtension));
}

auto ProjectMaskCacheService::PublishedSlotPath(const ProjectMaskCacheSlotIdentity& identity) const
    -> std::filesystem::path {
  std::lock_guard lock(mutex_);
  return SlotPath(chosen_root_, identity);
}

auto ProjectMaskCacheService::EnqueueSettledWrite(ProjectMaskCacheRecord record, std::string* error)
    -> bool {
  try {
    ValidateProjectMaskCacheRecord(record);
  } catch (const std::exception& exception) {
    return AssignError(error, exception.what());
  }
  std::lock_guard lock(mutex_);
  if (maintenance_) {
    return AssignError(error, "Project Mask cache is under maintenance");
  }
  if (!ChosenRootUsable(error)) {
    has_dirty_slot_ = true;
    last_error_     = error ? *error : last_error_;
    return false;
  }
  const auto key      = SlotKeyFrom(record.identity);
  pending_[key] = PendingWrite{std::move(record), generation_, chosen_root_};
  has_dirty_slot_     = true;
  cv_.notify_all();
  return true;
}

auto ProjectMaskCacheService::FlushPendingWrites(std::string* error) -> bool {
  std::unique_lock lock(mutex_);
  cv_.notify_all();
  cv_.wait(lock, [this] { return pending_.empty() && inflight_writes_ == 0; });
  if (!last_error_.empty() && has_dirty_slot_) {
    return AssignError(error, last_error_);
  }
  return true;
}

auto ProjectMaskCacheService::TryAcquireReader(const ProjectMaskCacheSlotIdentity& identity,
                                               std::string_view expected_fingerprint,
                                               std::string_view expected_producer,
                                               std::string* error) -> std::optional<ProjectMaskCacheReader> {
  std::filesystem::path path;
  {
    std::unique_lock lock(mutex_);
    if (maintenance_) {
      if (error) {
        error->clear();
      }
      return std::nullopt;
    }
    if (!ChosenRootUsable(error)) {
      return std::nullopt;
    }
    path = SlotPath(chosen_root_, identity);
    ++reader_count_;
  }

  struct CountGuard {
    ProjectMaskCacheService* service;
    ~CountGuard() {
      if (service) {
        service->ReleaseReader();
      }
    }
  } guard{this};

  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error) || exists_error) {
    if (error) {
      error->clear();
    }
    return std::nullopt;
  }
  if (std::filesystem::is_symlink(path)) {
    if (error) {
      error->clear();
    }
    return std::nullopt;
  }

  try {
    const auto bytes  = ReadEntireFile(path);
    auto       record = DecodeRecord(bytes, project_uuid_);
    if (record.identity.image_id != identity.image_id ||
        record.identity.node_id.Value() != identity.node_id.Value() ||
        record.recipe_fingerprint != expected_fingerprint ||
        record.producer_id != expected_producer) {
      if (error) {
        error->clear();
      }
      return std::nullopt;
    }
    ProjectMaskCacheReader reader;
    reader.record_  = std::make_shared<const ProjectMaskCacheRecord>(std::move(record));
    reader.release_ = [this] { ReleaseReader(); };
    guard.service   = nullptr;
    return reader;
  } catch (const std::exception&) {
    if (error) {
      error->clear();
    }
    return std::nullopt;
  }
}

auto ProjectMaskCacheService::ClearOwnedCache(std::string* error) -> bool {
  std::filesystem::path root;
  {
    std::unique_lock lock(mutex_);
    maintenance_ = true;
    ++generation_;
    pending_.clear();
    cv_.notify_all();
    cv_.wait(lock, [this] { return inflight_writes_ == 0 && reader_count_ == 0; });
    root = chosen_root_;
  }
  std::string delete_error;
  const bool  deleted = DeleteNamespaceFiles(root, &delete_error);
  {
    std::lock_guard lock(mutex_);
    maintenance_    = false;
    has_dirty_slot_ = false;
    if (deleted) {
      last_error_.clear();
    } else {
      last_error_ = delete_error;
    }
    cv_.notify_all();
  }
  if (!deleted) {
    return AssignError(error, delete_error);
  }
  return true;
}

auto ProjectMaskCacheService::PublishChosenRoot(const std::filesystem::path& new_root,
                                                std::string* error) -> bool {
  try {
    const auto normalized = NormalizeChosenRoot(new_root);
    std::unique_lock lock(mutex_);
    maintenance_ = true;
    ++generation_;
    pending_.clear();
    cv_.notify_all();
    cv_.wait(lock, [this] { return inflight_writes_ == 0; });
    chosen_root_    = normalized;
    maintenance_    = false;
    has_dirty_slot_ = false;
    last_error_.clear();
    cv_.notify_all();
    return true;
  } catch (const std::exception& exception) {
    return AssignError(error, exception.what());
  }
}

auto ProjectMaskCacheService::DeleteOwnedNamespace(const std::filesystem::path& chosen_root,
                                                   std::string* error) -> bool {
  return DeleteNamespaceFiles(chosen_root, error);
}

auto ProjectMaskCacheService::FenceWrites(std::string* error) -> bool {
  (void)error;
  std::unique_lock lock(mutex_);
  ++generation_;
  pending_.clear();
  cv_.notify_all();
  cv_.wait(lock, [this] { return inflight_writes_ == 0; });
  return true;
}

auto ProjectMaskCacheService::Usage() const -> ProjectMaskCacheUsage {
  std::lock_guard lock(mutex_);
  ProjectMaskCacheUsage usage;
  usage.pending_write_count = pending_.size();
  usage.has_dirty_slot      = has_dirty_slot_;
  usage.last_error          = last_error_;
  ScanUsage(&usage);
  return usage;
}

auto ProjectMaskCacheService::CountPublishedSlots() const -> std::size_t {
  return Usage().published_file_count;
}

void ProjectMaskCacheService::SetPublishHookForTesting(
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> hook) {
  std::lock_guard lock(mutex_);
  publish_hook_ = std::move(hook);
}

void ProjectMaskCacheService::ShutdownWriter() {
  {
    std::lock_guard lock(mutex_);
    stop_writer_ = true;
    cv_.notify_all();
  }
  if (writer_.joinable()) {
    writer_.join();
  }
}

void ProjectMaskCacheService::ReleaseReader() {
  std::lock_guard lock(mutex_);
  if (reader_count_ > 0) {
    --reader_count_;
  }
  cv_.notify_all();
}

auto ProjectMaskCacheService::ChosenRootUsable(std::string* error) const -> bool {
  std::error_code ec;
  if (std::filesystem::exists(chosen_root_, ec) && !std::filesystem::is_directory(chosen_root_, ec)) {
    return AssignError(error, "Configured project Mask cache root is not a directory");
  }
  return true;
}

void ProjectMaskCacheService::ScanUsage(ProjectMaskCacheUsage* usage) const {
  std::error_code ec;
  const auto      ns = ProjectMaskCacheNamespaceDirectory(chosen_root_, project_uuid_);
  if (!std::filesystem::exists(ns, ec) || !std::filesystem::is_directory(ns, ec)) {
    return;
  }
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(ns, options, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_symlink()) {
      continue;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!IsOwnedCacheFileName(entry.path())) {
      continue;
    }
    usage->published_file_count += 1;
    usage->published_byte_count += static_cast<std::size_t>(entry.file_size(ec));
  }
}

auto ProjectMaskCacheService::DeleteNamespaceFiles(const std::filesystem::path& chosen_root,
                                                   std::string* error) -> bool {
  std::error_code ec;
  const auto      ns = ProjectMaskCacheNamespaceDirectory(chosen_root, project_uuid_);
  if (!std::filesystem::exists(ns, ec)) {
    return true;
  }
  if (std::filesystem::is_symlink(ns)) {
    return RemovePath(ns, error);
  }
  if (!std::filesystem::is_directory(ns, ec)) {
    return AssignError(error, "Project Mask cache namespace is not a directory");
  }
  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> directories;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(ns, options, ec)) {
    if (ec) {
      return AssignError(error, "Failed to enumerate project Mask cache: " + ec.message());
    }
    if (entry.is_symlink()) {
      continue;
    }
    if (entry.is_directory()) {
      directories.push_back(entry.path());
      continue;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (IsOwnedTempFileName(name) || IsOwnedCacheFileName(entry.path())) {
      files.push_back(entry.path());
    }
  }
  for (const auto& file : files) {
    if (!RemovePath(file, error)) {
      return false;
    }
  }
  std::sort(directories.begin(), directories.end(),
            [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
              return PathText(lhs).size() > PathText(rhs).size();
            });
  for (const auto& directory : directories) {
    std::filesystem::remove(directory, ec);
  }
  std::filesystem::remove(ns, ec);
  return true;
}

auto ProjectMaskCacheService::WriteOne(const PendingWrite& pending, std::string* error) -> bool {
  if (!PrepareNamespace(pending.chosen_root, project_uuid_, error)) {
    return false;
  }
  const auto destination = SlotPath(pending.chosen_root, pending.record.identity);
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    return AssignError(error, "Failed to create Mix-cache slot directory: " + ec.message());
  }
  auto temporary = destination;
  temporary += ".tmp." + ProcessIdText() + "." + std::to_string(++g_temp_sequence);
  try {
    const auto bytes = EncodeRecord(project_uuid_, pending.record);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return AssignError(error, "Project Mask cache could not open a temporary file");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!FlushWrittenFile(stream, temporary, error)) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> hook;
    {
      std::lock_guard lock(mutex_);
      if (pending.generation != generation_) {
        std::filesystem::remove(temporary, ec);
        return AssignError(error, "Project Mask cache write was fenced");
      }
      hook = publish_hook_;
    }
    if (hook && !hook(temporary, destination)) {
      std::filesystem::remove(temporary, ec);
      return AssignError(error, "Project Mask cache publish hook rejected the write");
    }
    {
      std::lock_guard lock(mutex_);
      if (pending.generation != generation_) {
        std::filesystem::remove(temporary, ec);
        return AssignError(error, "Project Mask cache write was fenced");
      }
    }
    if (!AtomicReplaceFile(temporary, destination, error)) {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    std::filesystem::remove(temporary, ec);
    return AssignError(error, exception.what());
  }
}

void ProjectMaskCacheService::WriterLoop() {
  for (;;) {
    PendingWrite pending;
    bool         has_work = false;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_writer_ || !pending_.empty(); });
      if (stop_writer_ && pending_.empty()) {
        return;
      }
      if (!pending_.empty()) {
        pending = std::move(pending_.begin()->second);
        pending_.erase(pending_.begin());
        ++inflight_writes_;
        has_work = true;
      }
    }
    if (!has_work) {
      continue;
    }
    std::string write_error;
    const bool  ok = WriteOne(pending, &write_error);
    {
      std::lock_guard lock(mutex_);
      --inflight_writes_;
      if (ok) {
        if (pending_.empty()) {
          has_dirty_slot_ = false;
          last_error_.clear();
        }
      } else {
        has_dirty_slot_ = true;
        last_error_     = write_error;
      }
      cv_.notify_all();
    }
  }
}

}  // namespace alcedo
