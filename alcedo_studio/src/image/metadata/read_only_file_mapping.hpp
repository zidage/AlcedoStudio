//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace alcedo::metadata_detail {

/// Read-only memory mapping of a whole file.
///
/// The DNG tag parser reads a few IFDs and tag values that can sit anywhere in the file. Mapping
/// the file lets the OS read only those pages; copying the file read 40-100 MB per DNG and held
/// it per import worker.
class ReadOnlyFileMapping {
 public:
  explicit ReadOnlyFileMapping(const std::filesystem::path& path) {
#if defined(_WIN32)
    file_ = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      return;
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_, &file_size) || file_size.QuadPart <= 0) {
      return;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
      return;
    }
    const void* view = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
      return;
    }
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<size_t>(file_size.QuadPart);
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      return;
    }
    struct stat file_stat{};
    if (::fstat(fd_, &file_stat) != 0 || file_stat.st_size <= 0) {
      return;
    }
    void* view =
        ::mmap(nullptr, static_cast<size_t>(file_stat.st_size), PROT_READ, MAP_PRIVATE, fd_, 0);
    if (view == MAP_FAILED) {
      return;
    }
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<size_t>(file_stat.st_size);
#endif
  }

  ~ReadOnlyFileMapping() {
#if defined(_WIN32)
    if (data_ != nullptr) {
      UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
    }
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
    }
#else
    if (data_ != nullptr) {
      ::munmap(const_cast<uint8_t*>(data_), size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
  }

  ReadOnlyFileMapping(const ReadOnlyFileMapping&)            = delete;
  ReadOnlyFileMapping& operator=(const ReadOnlyFileMapping&) = delete;

  /// Empty when the file cannot be opened or mapped, or is empty.
  auto                 Bytes() const -> std::span<const uint8_t> { return {data_, size_}; }

 private:
#if defined(_WIN32)
  HANDLE file_    = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
  const uint8_t* data_ = nullptr;
  size_t         size_ = 0;
};

}  // namespace alcedo::metadata_detail
