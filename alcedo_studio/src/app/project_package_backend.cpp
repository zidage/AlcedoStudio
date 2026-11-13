//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/project_package_backend.hpp"

#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <json.hpp>
#include <xxhash.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#endif

#include "app/project_service.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/string/convert.hpp"

namespace alcedo::project_pack {

namespace album_util = ::alcedo::ui::album_util;

namespace {

auto Tr(const char* text) -> QString {
  return QCoreApplication::translate(ALCEDO_I18N_CONTEXT, text);
}

auto ReadExact(std::istream& stream, char* data, std::streamsize size) -> bool {
  stream.read(data, size);
  return stream.good() || stream.gcount() == size;
}

auto WriteU32Le(std::ostream& stream, uint32_t value) -> bool {
  std::array<unsigned char, 4> bytes{};
  bytes[0] = static_cast<unsigned char>(value & 0xFFU);
  bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  bytes[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
  bytes[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

auto WriteU64Le(std::ostream& stream, uint64_t value) -> bool {
  std::array<unsigned char, 8> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<unsigned char>((value >> (i * 8ULL)) & 0xFFULL);
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

auto ReadU32Le(std::istream& stream, uint32_t* value) -> bool {
  std::array<unsigned char, 4> bytes{};
  if (!ReadExact(stream, reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  *value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
  return true;
}

auto ReadU64Le(std::istream& stream, uint64_t* value) -> bool {
  std::array<unsigned char, 8> bytes{};
  if (!ReadExact(stream, reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  *value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    *value |= (static_cast<uint64_t>(bytes[i]) << (i * 8ULL));
  }
  return true;
}

auto ParseSemVer(std::string_view version, std::array<int, 3>* out) -> bool {
  std::array<int, 3> parts{};
  size_t             begin = 0;
  for (size_t index = 0; index < parts.size(); ++index) {
    const size_t end = version.find('.', begin);
    const auto   token =
        version.substr(begin, end == std::string_view::npos ? version.size() - begin : end - begin);
    if (token.empty()) {
      return false;
    }
    int value = 0;
    for (const char ch : token) {
      if (ch < '0' || ch > '9') {
        return false;
      }
      value = value * 10 + (ch - '0');
    }
    parts[index] = value;
    if (index + 1 < parts.size()) {
      if (end == std::string_view::npos) {
        return false;
      }
      begin = end + 1;
    } else if (end != std::string_view::npos) {
      return false;
    }
  }
  *out = parts;
  return true;
}

auto PackedProjectHeaderIsSupported(const std::filesystem::path& path) -> bool {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  std::array<char, kPackedProjectMagic.size()> magic{};
  if (!ReadExact(in, magic.data(), static_cast<std::streamsize>(magic.size())) ||
      !std::equal(magic.begin(), magic.end(), kPackedProjectMagic.begin())) {
    return false;
  }

  uint32_t version = 0;
  return ReadU32Le(in, &version) && version == kPackedProjectVersion;
}

constexpr size_t kPackedProjectCopyChunkBytes = 1024 * 1024;

using Xxh3State = std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)>;

auto MakeXxh3State() -> Xxh3State {
  Xxh3State state(XXH3_createState(), &XXH3_freeState);
  if (state && XXH3_64bits_reset(state.get()) == XXH_ERROR) {
    state.reset();
  }
  return state;
}

/**
 * @brief Starts the checksum stored in the packed project header.
 *
 * The checksum is XXH3-64 of magic (8B) || version (4B LE) || meta_size (8B LE) ||
 * db_size (8B LE) || meta bytes || db bytes. This feeds the fixed fields; the payload bytes
 * follow chunk by chunk as CopyPayloadChunks copies them.
 */
auto HashPackedProjectHeaderFields(XXH3_state_t* state, uint64_t meta_size, uint64_t db_size)
    -> bool {
  std::array<unsigned char, 4 + 8 + 8> sizes{};
  for (size_t i = 0; i < 4; ++i) {
    sizes[i] = static_cast<unsigned char>((kPackedProjectVersion >> (i * 8U)) & 0xFFU);
  }
  for (size_t i = 0; i < 8; ++i) {
    sizes[4 + i]  = static_cast<unsigned char>((meta_size >> (i * 8ULL)) & 0xFFULL);
    sizes[12 + i] = static_cast<unsigned char>((db_size >> (i * 8ULL)) & 0xFFULL);
  }
  return XXH3_64bits_update(state, kPackedProjectMagic.data(), kPackedProjectMagic.size()) !=
             XXH_ERROR &&
         XXH3_64bits_update(state, sizes.data(), sizes.size()) != XXH_ERROR;
}

enum class PayloadCopyResult : uint8_t { kCopied, kReadFailed, kWriteFailed, kHashFailed };

/// Copies exactly @p size bytes from @p in to @p out in chunks of @p buffer and feeds the same
/// bytes to @p state.
auto CopyPayloadChunks(std::istream& in, std::ostream& out, uint64_t size, XXH3_state_t* state,
                       std::vector<char>* buffer) -> PayloadCopyResult {
  uint64_t remaining = size;
  while (remaining > 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer->size())));
    if (!ReadExact(in, buffer->data(), chunk)) {
      return PayloadCopyResult::kReadFailed;
    }
    if (XXH3_64bits_update(state, buffer->data(), static_cast<size_t>(chunk)) == XXH_ERROR) {
      return PayloadCopyResult::kHashFailed;
    }
    out.write(buffer->data(), chunk);
    if (!out) {
      return PayloadCopyResult::kWriteFailed;
    }
    remaining -= static_cast<uint64_t>(chunk);
  }
  return PayloadCopyResult::kCopied;
}

/**
 * @brief Reads a packed project from its file: the header and the metadata first, then the
 * database payload as a stream.
 *
 * Open checks the header against the file size and reads the metadata, so the caller can
 * reject an unsupported metadata version before any database byte is written. The checksum
 * covers both payloads and is verified at the end of CopyDatabaseTo.
 */
class PackedProjectReader {
 public:
  auto Open(const std::filesystem::path& packed_path, QString* errorOut) -> bool {
    auto fail = [errorOut](const QString& text) {
      if (errorOut) {
        *errorOut = text;
      }
      return false;
    };

    std::error_code ec;
    const uint64_t  file_size = std::filesystem::file_size(packed_path, ec);
    in_.open(packed_path, std::ios::binary);
    if (ec || !in_.is_open()) {
      if (errorOut) {
        *errorOut =
            Tr("Failed to open packed project: %1").arg(album_util::PathToQString(packed_path));
      }
      return false;
    }

    std::array<char, kPackedProjectMagic.size()> magic{};
    if (!ReadExact(in_, magic.data(), static_cast<std::streamsize>(magic.size())) ||
        !std::equal(magic.begin(), magic.end(), kPackedProjectMagic.begin())) {
      return fail(Tr("Packed project signature is invalid."));
    }

    uint32_t version = 0;
    if (!ReadU32Le(in_, &version) || version != kPackedProjectVersion) {
      return fail(Tr("Packed project version is not supported."));
    }

    uint64_t meta_size = 0;
    if (!ReadU64Le(in_, &meta_size) || !ReadU64Le(in_, &db_size_) || !ReadU64Le(in_, &checksum_)) {
      return fail(Tr("Packed project header is corrupted."));
    }
    // The header sizes must describe the file exactly. The sums are checked in this order so
    // that no subtraction can wrap. A file shorter than the header failed the reads above.
    if (file_size < kPackedProjectHeaderBytes) {
      return fail(Tr("Packed project header is corrupted."));
    }
    const uint64_t payload_bytes = file_size - kPackedProjectHeaderBytes;
    if (meta_size > payload_bytes || db_size_ > payload_bytes - meta_size) {
      return fail(Tr("Packed project header is corrupted."));
    }
    if (db_size_ < payload_bytes - meta_size) {
      return fail(Tr("Packed project contains unexpected trailing data."));
    }
    if (meta_size > kMaxPackedMetadataBytes) {
      return fail(Tr("Packed project metadata is too large."));
    }

    state_ = MakeXxh3State();
    if (!state_ || !HashPackedProjectHeaderFields(state_.get(), meta_size, db_size_)) {
      return fail(Tr("Failed to compute packed project checksum."));
    }

    meta_.assign(static_cast<size_t>(meta_size), '\0');
    if (meta_size > 0 && !ReadExact(in_, meta_.data(), static_cast<std::streamsize>(meta_size))) {
      return fail(Tr("Failed to read packed metadata payload."));
    }
    if (XXH3_64bits_update(state_.get(), meta_.data(), meta_.size()) == XXH_ERROR) {
      return fail(Tr("Failed to compute packed project checksum."));
    }
    return true;
  }

  [[nodiscard]] auto metadata() const -> const std::string& { return meta_; }

  /// Copies the database payload to @p db_path and verifies the checksum. A failure removes
  /// @p db_path.
  auto CopyDatabaseTo(const std::filesystem::path& db_path, QString* errorOut) -> bool {
    const bool copied = CopyAndVerify(db_path, errorOut);
    if (!copied) {
      std::error_code ec;
      std::filesystem::remove(db_path, ec);
    }
    return copied;
  }

 private:
  auto CopyAndVerify(const std::filesystem::path& db_path, QString* errorOut) -> bool {
    std::ofstream out(db_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      if (errorOut) {
        *errorOut = Tr("Failed to materialize project database: %1")
                        .arg(album_util::PathToQString(db_path));
      }
      return false;
    }
    std::vector<char> buffer(kPackedProjectCopyChunkBytes);
    const auto        result = CopyPayloadChunks(in_, out, db_size_, state_.get(), &buffer);
    out.close();
    if (result == PayloadCopyResult::kReadFailed) {
      if (errorOut) {
        *errorOut = Tr("Failed to read packed database payload.");
      }
      return false;
    }
    if (result != PayloadCopyResult::kCopied || out.fail()) {
      if (errorOut) {
        *errorOut = result == PayloadCopyResult::kHashFailed
                        ? Tr("Failed to compute packed project checksum.")
                        : Tr("Failed to materialize project database: %1")
                              .arg(album_util::PathToQString(db_path));
      }
      return false;
    }
    if (XXH3_64bits_digest(state_.get()) != checksum_) {
      if (errorOut) {
        *errorOut = Tr("Packed project checksum verification failed.");
      }
      return false;
    }
    return true;
  }

  std::ifstream in_{};
  uint64_t      db_size_  = 0;
  uint64_t      checksum_ = 0;
  std::string   meta_{};
  Xxh3State     state_{nullptr, &XXH3_freeState};
};

}  // namespace

auto IsMetadataJsonPath(const std::filesystem::path& path) -> bool {
  std::wstring ext = path.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  return ext == L".json";
}

auto IsPackedProjectPath(const std::filesystem::path& path) -> bool {
  std::wstring ext = path.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  return ext == kPackedProjectExtension;
}

auto IsPackedProjectFile(const std::filesystem::path& path) -> bool {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  std::array<char, kPackedProjectMagic.size()> magic{};
  if (!ReadExact(in, magic.data(), static_cast<std::streamsize>(magic.size()))) {
    return false;
  }
  return std::equal(magic.begin(), magic.end(), kPackedProjectMagic.begin());
}

auto IsSupportedProjectFile(const std::filesystem::path& path) -> bool {
  return IsPackedProjectPath(path) && PackedProjectHeaderIsSupported(path);
}

auto ReadFileBytes(const std::filesystem::path& path, std::string* out) -> bool {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out->assign(static_cast<size_t>(size), '\0');
  if (size == 0) {
    return true;
  }
  return ReadExact(in, out->data(), size);
}

auto WriteFileBytes(const std::filesystem::path& path, const std::string& data) -> bool {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  if (!data.empty()) {
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  return static_cast<bool>(out);
}

auto ComputeFileChecksum(const std::filesystem::path& path, uint64_t* checksumOut) -> bool {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  XXH3_state_t* state = XXH3_createState();
  if (state == nullptr) {
    return false;
  }
  if (XXH3_64bits_reset(state) == XXH_ERROR) {
    XXH3_freeState(state);
    return false;
  }

  std::vector<char> buffer(1024 * 1024);
  while (in.good()) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read_count = in.gcount();
    if (read_count > 0 &&
        XXH3_64bits_update(state, buffer.data(), static_cast<size_t>(read_count)) == XXH_ERROR) {
      XXH3_freeState(state);
      return false;
    }
  }
  if (!in.eof()) {
    XXH3_freeState(state);
    return false;
  }

  *checksumOut = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return true;
}

auto FormatChecksum(uint64_t checksum) -> std::string {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << checksum;
  return out.str();
}

auto ProjectVersionIsSupported(std::string_view version) -> bool {
  std::array<int, 3> parsed{};
  std::array<int, 3> min_supported{};
  std::array<int, 3> max_supported{};
  return ParseSemVer(version, &parsed) &&
         ParseSemVer(kMinSupportedProjectFileVersion, &min_supported) &&
         ParseSemVer(kMaxSupportedProjectFileVersion, &max_supported) &&
         parsed >= min_supported && parsed <= max_supported;
}

auto BuildUniquePackedProjectPath(const std::filesystem::path& folder,
                                  const QString& projectName,
                                  QString* errorOut) -> std::optional<std::filesystem::path> {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec)) {
    std::filesystem::create_directories(folder, ec);
  }
  if (ec || !std::filesystem::is_directory(folder, ec) || ec) {
    if (errorOut) {
      *errorOut = Tr("Selected folder is invalid or not writable.");
    }
    return std::nullopt;
  }

  const auto valid_name = album_util::ValidateProjectName(projectName, errorOut);
  if (!valid_name.has_value()) {
    return std::nullopt;
  }

  QString base_name_text = valid_name.value();
  if (base_name_text.endsWith(".alcd", Qt::CaseInsensitive)) {
    base_name_text.chop(QString(".alcd").size());
    base_name_text = base_name_text.trimmed();
  }
  if (base_name_text.isEmpty()) {
    if (errorOut) {
      *errorOut = Tr("Project name cannot be only an extension.");
    }
    return std::nullopt;
  }

  const std::wstring base_name = base_name_text.toStdWString();
  for (int index = 0; index < 1024; ++index) {
    std::wstring suffix;
    if (index > 0) {
      suffix = L"_" + std::to_wstring(index);
    }

    const auto packed_path = folder / (base_name + suffix + std::wstring(kPackedProjectExtension));
    ec.clear();
    const bool exists = std::filesystem::exists(packed_path, ec);
    if (ec) {
      continue;
    }
    if (!exists) {
      return packed_path;
    }
  }

  if (errorOut) {
    *errorOut = Tr("A packed project with that name already exists.");
  }
  return std::nullopt;
}

auto BuildBundlePathFromMetaPath(const std::filesystem::path& metaPath) -> std::filesystem::path {
  if (metaPath.empty()) {
    return {};
  }
  return metaPath.parent_path() /
         (metaPath.stem().wstring() + std::wstring(kPackedProjectExtension));
}

ProjectWorkspaceLock::~ProjectWorkspaceLock() { Release(); }

ProjectWorkspaceLock::ProjectWorkspaceLock(ProjectWorkspaceLock&& other) noexcept {
  *this = std::move(other);
}

auto ProjectWorkspaceLock::operator=(ProjectWorkspaceLock&& other) noexcept
    -> ProjectWorkspaceLock& {
  if (this != &other) {
    Release();
#ifdef _WIN32
    handle_       = other.handle_;
    other.handle_ = nullptr;
#else
    fd_       = other.fd_;
    other.fd_ = -1;
#endif
  }
  return *this;
}

auto ProjectWorkspaceLock::Acquire(const std::filesystem::path& workspace, QString* errorOut)
    -> bool {
  Release();
  const auto lock_path = workspace / std::filesystem::path(kProjectWorkspaceLockFileName);
#ifdef _WIN32
  HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    handle_ = handle;
    return true;
  }
#else
  const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0) {
    fd_ = fd;
    return true;
  }
  if (fd >= 0) {
    close(fd);
  }
#endif
  if (errorOut) {
    *errorOut =
        Tr("Unable to lock project temp directory: %1").arg(album_util::PathToQString(lock_path));
  }
  return false;
}

auto ProjectWorkspaceLock::held() const -> bool {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

void ProjectWorkspaceLock::Release() {
#ifdef _WIN32
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    close(fd_);  // also releases the flock
    fd_ = -1;
  }
#endif
}

auto ProjectTempRoot() -> std::filesystem::path {
  const QString temp_dir_text =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation).isEmpty()
          ? QDir::tempPath()
          : QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  return album_util::QStringToFsPath(temp_dir_text) / L"alcedo_main";
}

namespace {

// Captured when this library is loaded at process start, before the process creates any
// workspace or snapshot. The sweep leaves every entry modified after it alone.
const std::filesystem::file_time_type kProcessStartFileTime =
    std::filesystem::file_time_type::clock::now();

auto PathText(const std::filesystem::path& path) -> QString {
  return album_util::PathToQString(path);
}

#ifdef _WIN32
// Every open with data access conflicts with share mode 0, whatever its own share mode.
auto FileIsOpenElsewhere(const std::filesystem::path& path) -> bool {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_SHARING_VIOLATION;
  }
  CloseHandle(handle);
  return false;
}
#endif

auto OwnerLockIsHeld(const std::filesystem::path& lock_path) -> bool {
#ifdef _WIN32
  return FileIsOpenElsewhere(lock_path);
#else
  const int fd = open(lock_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
  close(fd);
  return held;
#endif
}

// DuckDB holds its database file open (Windows) and with an fcntl write lock (macOS). On macOS
// F_GETLK reports the locks of other processes only; the sweep never reads a file that this
// process opened, because such a file is newer than kProcessStartFileTime.
auto DatabaseFileIsHeld(const std::filesystem::path& db_path) -> bool {
#ifdef _WIN32
  return FileIsOpenElsewhere(db_path);
#else
  const int fd = open(db_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct flock lock_query{};
  lock_query.l_type   = F_WRLCK;
  lock_query.l_whence = SEEK_SET;
  const bool held     = fcntl(fd, F_GETLK, &lock_query) == 0 && lock_query.l_type != F_UNLCK;
  close(fd);
  return held;
#endif
}

auto WorkspaceIsInUse(const std::filesystem::path& workspace) -> bool {
  if (OwnerLockIsHeld(workspace / std::filesystem::path(kProjectWorkspaceLockFileName))) {
    return true;
  }
  // A workspace of an Alcedo version without owner locks: its open database tells that it is
  // live.
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(workspace, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    std::error_code type_error;
    if (it->is_regular_file(type_error) && it->path().extension() == ".db" &&
        DatabaseFileIsHeld(it->path())) {
      return true;
    }
  }
  return false;
}

auto StartsWith(const std::wstring& text, std::wstring_view prefix) -> bool {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

auto EndsWith(const std::wstring& text, std::wstring_view suffix) -> bool {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

void RemoveAbandonedProjectTempFiles(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) {
    return;
  }
  std::vector<std::filesystem::directory_entry> entries;
  for (auto it = std::filesystem::directory_iterator(root, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    entries.push_back(*it);
  }
  if (ec) {
    qCWarning(alcedo::diag::projectLog).noquote()
        << "Project temp sweep cannot list" << PathText(root)
        << QString::fromStdString(ec.message());
  }

  for (const auto& entry : entries) {
    const std::wstring name         = entry.path().filename().wstring();
    const bool         is_workspace = StartsWith(name, L"runtime_");
    const bool         is_snapshot  = StartsWith(name, L"pack_snapshot_") &&
                             (EndsWith(name, L".db") || EndsWith(name, L".db.wal"));
    if (!is_workspace && !is_snapshot) {
      continue;
    }
    std::error_code entry_error;
    const auto      modified = std::filesystem::last_write_time(entry.path(), entry_error);
    if (entry_error || modified >= kProcessStartFileTime) {
      continue;
    }

    if (is_workspace) {
      if (!entry.is_directory(entry_error) || entry_error || WorkspaceIsInUse(entry.path())) {
        continue;
      }
      if (album_util::CleanupWorkspaceDirectory(entry.path())) {
        qCInfo(alcedo::diag::projectLog).noquote()
            << "Removed abandoned runtime workspace:" << PathText(entry.path());
      }
      continue;
    }

    if (!entry.is_regular_file(entry_error) || entry_error || DatabaseFileIsHeld(entry.path())) {
      continue;
    }
    std::error_code remove_error;
    if (std::filesystem::remove(entry.path(), remove_error)) {
      qCInfo(alcedo::diag::projectLog).noquote()
          << "Removed abandoned DB snapshot:" << PathText(entry.path());
    } else if (remove_error) {
      qCWarning(alcedo::diag::projectLog).noquote()
          << "Abandoned DB snapshot removal failed:" << PathText(entry.path())
          << QString::fromStdString(remove_error.message());
    }
  }
}

auto CreateProjectWorkspace(const QString& projectName, std::filesystem::path* workspaceOut,
                            ProjectWorkspaceLock* lockOut, QString* errorOut) -> bool {
  const auto root = ProjectTempRoot();
  RemoveAbandonedProjectTempFiles(root);
  if (!album_util::EnsureDirectoryExists(root)) {
    if (errorOut) {
      *errorOut = Tr("Unable to create project temp directory.");
    }
    return false;
  }

  QString normalized_name = projectName.trimmed();
  if (normalized_name.isEmpty()) {
    normalized_name = "project";
  }
  for (QChar& ch : normalized_name) {
    if (!(ch.isLetterOrNumber() || ch == '_' || ch == '-')) {
      ch = '_';
    }
  }

  const auto stamp = static_cast<unsigned long long>(QDateTime::currentMSecsSinceEpoch());
  for (int attempt = 0; attempt < 128; ++attempt) {
    const auto nonce =
        static_cast<unsigned long long>(QRandomGenerator::global()->generate64());
    const std::wstring dir_name =
        QString("runtime_%1_%2_%3")
            .arg(normalized_name)
            .arg(QString::number(stamp))
            .arg(QString::number(nonce + static_cast<unsigned long long>(attempt)))
            .toStdWString();

    const auto candidate = root / dir_name;
    std::error_code ec;
    if (!std::filesystem::create_directory(candidate, ec) || ec) {
      continue;
    }
    if (!lockOut->Acquire(candidate, errorOut)) {
      std::filesystem::remove_all(candidate, ec);
      return false;
    }
    *workspaceOut = candidate;
    return true;
  }

  if (errorOut) {
    *errorOut = Tr("Unable to allocate a unique project temp directory.");
  }
  return false;
}

auto BuildRuntimeProjectPair(const std::filesystem::path& workspace,
                             const QString& projectName)
    -> std::pair<std::filesystem::path, std::filesystem::path> {
  QString base = projectName.trimmed();
  if (base.isEmpty()) {
    base = "album_editor_project";
  }
  if (base.endsWith(".alcd", Qt::CaseInsensitive)) {
    base.chop(QString(".alcd").size());
    base = base.trimmed();
  }
  auto valid_name = album_util::ValidateProjectName(base, nullptr);
  if (valid_name.has_value()) {
    base = valid_name.value();
  } else {
    base = "album_editor_project";
  }
  const std::wstring stem = base.toStdWString();
  return {workspace / (stem + L".db"), workspace / (stem + L".json")};
}

auto BuildTempDbSnapshotPath(std::filesystem::path* snapshotPathOut,
                             QString* errorOut) -> bool {
  const auto root = ProjectTempRoot();
  if (!album_util::EnsureDirectoryExists(root)) {
    if (errorOut) {
      *errorOut = Tr("Unable to prepare temp folder for DB snapshot.");
    }
    return false;
  }

  const auto stamp = static_cast<unsigned long long>(QDateTime::currentMSecsSinceEpoch());
  for (int attempt = 0; attempt < 128; ++attempt) {
    const auto nonce =
        static_cast<unsigned long long>(QRandomGenerator::global()->generate64());
    const auto candidate =
        root / QString("pack_snapshot_%1_%2.db")
                   .arg(QString::number(stamp))
                   .arg(QString::number(nonce + static_cast<unsigned long long>(attempt)))
                   .toStdWString();
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) || ec) {
      *snapshotPathOut = candidate;
      return true;
    }
  }

  if (errorOut) {
    *errorOut = Tr("Unable to allocate temp DB snapshot path.");
  }
  return false;
}

auto CreateLiveDbSnapshot(const std::shared_ptr<ProjectService>& project,
                          const std::filesystem::path& snapshotPath,
                          QString* errorOut) -> bool {
  if (!project) {
    if (errorOut) {
      *errorOut = Tr("No active project for DB snapshot.");
    }
    return false;
  }

  try {
    auto storage = project->GetStorage();
    if (!storage) {
      if (errorOut) {
        *errorOut = Tr("Storage service is unavailable for DB snapshot.");
      }
      return false;
    }

    std::error_code ec;
    auto            wal_path = snapshotPath;
    wal_path += ".wal";
    auto remove_snapshot_files = [&snapshotPath, &wal_path]() {
      std::error_code remove_error;
      std::filesystem::remove(snapshotPath, remove_error);
      std::filesystem::remove(wal_path, remove_error);
    };
    remove_snapshot_files();

    try {
      storage->Maintenance().WriteSnapshot(snapshotPath);
    } catch (const std::exception& e) {
      if (errorOut) {
        *errorOut = Tr("DuckDB snapshot failed: %1").arg(QString::fromUtf8(e.what()));
      }
      remove_snapshot_files();
      return false;
    }

    ec.clear();
    if (!std::filesystem::is_regular_file(snapshotPath, ec) || ec) {
      if (errorOut) {
        *errorOut = Tr("DuckDB snapshot file was not created: %1")
                        .arg(album_util::PathToQString(snapshotPath));
      }
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    if (errorOut) {
      *errorOut = Tr("DuckDB snapshot failed: %1").arg(QString::fromUtf8(e.what()));
    }
    return false;
  } catch (...) {
    if (errorOut) {
      *errorOut = Tr("DuckDB snapshot failed with an unknown error.");
    }
    return false;
  }
}

namespace {

/// Writes the complete package to @p temp_path. The checksum field is written as zero first and
/// filled in after both payloads were copied through the hash state.
auto WritePackedProjectFile(const std::filesystem::path& temp_path,
                            const std::filesystem::path& meta_path,
                            const std::filesystem::path& db_path, QString* errorOut) -> bool {
  std::ifstream   meta_in(meta_path, std::ios::binary);
  std::error_code ec;
  const uint64_t  meta_size = std::filesystem::file_size(meta_path, ec);
  if (!meta_in.is_open() || ec) {
    if (errorOut) {
      *errorOut =
          Tr("Failed to read project metadata: %1").arg(album_util::PathToQString(meta_path));
    }
    return false;
  }
  std::ifstream  db_in(db_path, std::ios::binary);
  const uint64_t db_size = std::filesystem::file_size(db_path, ec);
  if (!db_in.is_open() || ec) {
    if (errorOut) {
      *errorOut = Tr("Failed to read project database: %1").arg(album_util::PathToQString(db_path));
    }
    return false;
  }

  auto state = MakeXxh3State();
  if (!state || !HashPackedProjectHeaderFields(state.get(), meta_size, db_size)) {
    if (errorOut) {
      *errorOut = Tr("Failed to compute packed project checksum.");
    }
    return false;
  }

  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (errorOut) {
      *errorOut = Tr("Failed to open packed project file for writing: %1")
                      .arg(album_util::PathToQString(temp_path));
    }
    return false;
  }

  out.write(kPackedProjectMagic.data(),
            static_cast<std::streamsize>(kPackedProjectMagic.size()));
  if (!out || !WriteU32Le(out, kPackedProjectVersion) || !WriteU64Le(out, meta_size) ||
      !WriteU64Le(out, db_size) || !WriteU64Le(out, 0)) {
    if (errorOut) {
      *errorOut = Tr("Failed to write packed project header.");
    }
    return false;
  }

  std::vector<char> buffer(kPackedProjectCopyChunkBytes);
  const struct {
    std::ifstream*               in;
    uint64_t                     size;
    const std::filesystem::path* path;
    const char*                  read_error;
  } payloads[] = {
      {&meta_in, meta_size, &meta_path,
       QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, "Failed to read project metadata: %1")},
      {&db_in, db_size, &db_path,
       QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, "Failed to read project database: %1")},
  };
  for (const auto& payload : payloads) {
    const auto result = CopyPayloadChunks(*payload.in, out, payload.size, state.get(), &buffer);
    if (result == PayloadCopyResult::kCopied) {
      continue;
    }
    if (errorOut) {
      switch (result) {
        case PayloadCopyResult::kReadFailed:
          *errorOut = Tr(payload.read_error).arg(album_util::PathToQString(*payload.path));
          break;
        case PayloadCopyResult::kHashFailed:
          *errorOut = Tr("Failed to compute packed project checksum.");
          break;
        default:
          *errorOut = Tr("Failed to write packed project payload.");
          break;
      }
    }
    return false;
  }

  out.seekp(static_cast<std::streamoff>(kPackedProjectChecksumOffset));
  if (!out || !WriteU64Le(out, XXH3_64bits_digest(state.get()))) {
    if (errorOut) {
      *errorOut = Tr("Failed to write packed project header.");
    }
    return false;
  }
  out.close();
  if (out.fail()) {
    if (errorOut) {
      *errorOut = Tr("Failed to write packed project payload.");
    }
    return false;
  }
  return true;
}

}  // namespace

auto WritePackedProject(const std::filesystem::path& packedPath,
                        const std::filesystem::path& metaPath, const std::filesystem::path& dbPath,
                        QString* errorOut) -> bool {
  if (!album_util::EnsureDirectoryExists(packedPath.parent_path())) {
    if (errorOut) {
      *errorOut = Tr("Packed project destination folder is not writable.");
    }
    return false;
  }

  auto temp_path = packedPath;
  temp_path += ".tmp";
  std::error_code ec;
  if (!WritePackedProjectFile(temp_path, metaPath, dbPath, errorOut)) {
    std::filesystem::remove(temp_path, ec);
    return false;
  }

  if (std::filesystem::exists(packedPath, ec) && !ec) {
    std::filesystem::remove(packedPath, ec);
    if (ec) {
      if (errorOut) {
        *errorOut = Tr("Failed to replace existing packed project file: %1")
                        .arg(album_util::PathToQString(packedPath));
      }
      std::filesystem::remove(temp_path, ec);
      return false;
    }
  }
  ec.clear();
  std::filesystem::rename(temp_path, packedPath, ec);
  if (ec) {
    if (errorOut) {
      *errorOut = Tr("Failed to finalize packed project file: %1")
                      .arg(album_util::PathToQString(packedPath));
    }
    std::filesystem::remove(temp_path, ec);
    return false;
  }

  return true;
}

auto ReadPackedProject(const std::filesystem::path& packedPath, std::string* metaBytes,
                       const std::filesystem::path& dbOutPath, QString* errorOut) -> bool {
  PackedProjectReader reader;
  if (!reader.Open(packedPath, errorOut) || !reader.CopyDatabaseTo(dbOutPath, errorOut)) {
    return false;
  }
  *metaBytes = reader.metadata();
  return true;
}

auto UnpackProjectToWorkspace(const std::filesystem::path& packedPath,
                              const std::filesystem::path& workspaceDir,
                              const QString& projectName,
                              std::filesystem::path* dbPathOut,
                              std::filesystem::path* metaPathOut,
                              QString* errorOut) -> bool {
  PackedProjectReader reader;
  if (!reader.Open(packedPath, errorOut)) {
    return false;
  }

  const auto runtime_pair = BuildRuntimeProjectPair(workspaceDir, projectName);
  const auto db_path      = runtime_pair.first;
  const auto meta_path    = runtime_pair.second;

  // The metadata version is read before the checksum is verified, so an unsupported package
  // is rejected before its database payload is copied. The checksum is verified before any
  // metadata value is used.
  nlohmann::json metadata;
  try {
    metadata = nlohmann::json::parse(reader.metadata());
  } catch (...) {
    if (errorOut) {
      *errorOut = Tr("Packed project metadata JSON is invalid.");
    }
    return false;
  }
  if (!metadata.is_object() || !metadata.contains("project_file_version") ||
      !metadata.at("project_file_version").is_string()) {
    if (errorOut) {
      *errorOut = Tr("Incompatible project format: packed project metadata version is missing.");
    }
    return false;
  }
  if (!ProjectVersionIsSupported(metadata.at("project_file_version").get<std::string>())) {
    if (errorOut) {
      *errorOut = Tr("Incompatible project format: packed project metadata version is not "
                     "supported. Older project packages are not migrated.");
    }
    return false;
  }

  metadata["db_path"]   = conv::ToBytes(db_path.wstring());
  metadata["meta_path"] = conv::ToBytes(meta_path.wstring());

  std::error_code ec;
  std::filesystem::create_directories(db_path.parent_path(), ec);
  if (ec) {
    if (errorOut) {
      *errorOut = Tr("Failed to prepare temp files for packed project.");
    }
    return false;
  }

  if (!reader.CopyDatabaseTo(db_path, errorOut)) {
    return false;
  }

  const std::string meta_text = metadata.dump(4);
  if (!WriteFileBytes(meta_path, meta_text)) {
    if (errorOut) {
      *errorOut = Tr("Failed to materialize project metadata: %1")
                      .arg(album_util::PathToQString(meta_path));
    }
    return false;
  }

  *dbPathOut   = db_path;
  *metaPathOut = meta_path;
  return true;
}

}  // namespace alcedo::project_pack
