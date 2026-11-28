//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/file/source_file_fingerprint.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace alcedo {
namespace {

/**
 * @brief A source file opened for read only.
 *
 * The file is opened with the platform API instead of a stream: on Windows, a stream opens the
 * file without FILE_SHARE_DELETE, and the user cannot rename or delete the file in the file
 * manager during the read. The application never blocks a change of a user file (Mounted Folder
 * Plan, D1).
 */
class ReadOnlySourceFile {
 public:
  explicit ReadOnlySourceFile(const std::filesystem::path& path) {
#ifdef _WIN32
    handle_ = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      BY_HANDLE_FILE_INFORMATION info{};
      // A directory opens with GENERIC_READ only with FILE_FLAG_BACKUP_SEMANTICS; the check is
      // for other reparse targets that open but are not files.
      if (!::GetFileInformationByHandle(handle_, &info) ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
      }
    }
#else
    descriptor_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor_ >= 0) {
      struct stat info{};
      if (::fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
    }
#endif
  }

  ~ReadOnlySourceFile() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
#else
    if (descriptor_ >= 0) ::close(descriptor_);
#endif
  }

  ReadOnlySourceFile(const ReadOnlySourceFile&)                                  = delete;
  auto               operator=(const ReadOnlySourceFile&) -> ReadOnlySourceFile& = delete;

  [[nodiscard]] auto IsOpen() const -> bool {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return descriptor_ >= 0;
#endif
  }

  /// Current byte size of the open file, or empty when the size cannot be read.
  [[nodiscard]] auto ByteSize() const -> std::optional<std::uint64_t> {
#ifdef _WIN32
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle_, &size) || size.QuadPart < 0) return std::nullopt;
    return static_cast<std::uint64_t>(size.QuadPart);
#else
    struct stat info{};
    if (::fstat(descriptor_, &info) != 0 || info.st_size < 0) return std::nullopt;
    return static_cast<std::uint64_t>(info.st_size);
#endif
  }

  /// Read exactly @p size bytes at @p offset into @p buffer. False on an error or a short read.
  [[nodiscard]] auto ReadAt(std::uint64_t offset, std::byte* buffer, std::size_t size) const
      -> bool {
    std::size_t done = 0;
    while (done < size) {
#ifdef _WIN32
      OVERLAPPED position{};
      const auto at       = offset + done;
      position.Offset     = static_cast<DWORD>(at & 0xFFFFFFFFu);
      position.OffsetHigh = static_cast<DWORD>(at >> 32);
      DWORD read          = 0;
      if (!::ReadFile(handle_, buffer + done, static_cast<DWORD>(size - done), &read, &position) ||
          read == 0) {
        return false;
      }
#else
      const auto read =
          ::pread(descriptor_, buffer + done, size - done, static_cast<off_t>(offset + done));
      if (read < 0 && errno == EINTR) continue;
      if (read <= 0) return false;
#endif
      done += static_cast<std::size_t>(read);
    }
    return true;
  }

 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

auto Error(SourceFileFingerprintError error) -> SourceFileFingerprintResult {
  return SourceFileFingerprintResult{.error = error, .fingerprint = {}};
}

}  // namespace

auto ComputeSourceFileFingerprint(const std::filesystem::path& path)
    -> SourceFileFingerprintResult {
  const ReadOnlySourceFile file(path);
  if (!file.IsOpen()) return Error(SourceFileFingerprintError::kCannotOpen);
  const auto size_before = file.ByteSize();
  if (!size_before) return Error(SourceFileFingerprintError::kReadFailed);
  const std::uint64_t     byte_size = *size_before;

  // A file of two blocks or less is read in full, so no byte contributes two times.
  constexpr std::uint64_t kBlock    = kSourceFileFingerprintBlockSize;
  const bool              whole     = byte_size <= 2 * kBlock;
  const std::size_t       head_size =
      whole ? static_cast<std::size_t>(byte_size) : kSourceFileFingerprintBlockSize;
  const std::size_t      tail_size   = whole ? 0 : kSourceFileFingerprintBlockSize;

  // The hashed bytes: the 8-byte little-endian byte size, the first block, the last block.
  constexpr std::size_t  kSizePrefix = 8;
  std::vector<std::byte> bytes(kSizePrefix + head_size + tail_size);
  for (std::size_t i = 0; i < kSizePrefix; ++i) {
    bytes[i] = static_cast<std::byte>((byte_size >> (8 * i)) & 0xFFu);
  }
  const bool read =
      file.ReadAt(0, bytes.data() + kSizePrefix, head_size) &&
      (tail_size == 0 ||
       file.ReadAt(byte_size - kBlock, bytes.data() + kSizePrefix + head_size, tail_size));
  // A file that another process shortened gives a short read; the size check names that case.
  const auto size_after = file.ByteSize();
  if (size_after && *size_after != byte_size) {
    return Error(SourceFileFingerprintError::kSizeChangedDuringRead);
  }
  if (!read || !size_after) return Error(SourceFileFingerprintError::kReadFailed);

  return SourceFileFingerprintResult{
      .error       = SourceFileFingerprintError::kNone,
      .fingerprint = SourceFileFingerprint{.hash    = Hash128::Compute(bytes.data(), bytes.size()),
                                           .version = kSourceFileFingerprintVersion}};
}

}  // namespace alcedo
