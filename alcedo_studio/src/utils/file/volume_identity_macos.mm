//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#import <Foundation/Foundation.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "utils/file/volume_identity.hpp"

namespace alcedo {

auto ReadVolumeIdentity(const std::filesystem::path& directory) -> std::string {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:directory.c_str()];
    if (path == nil) return {};
    NSURL*    url  = [NSURL fileURLWithPath:path isDirectory:YES];
    NSString* uuid = nil;
    // A network volume and some external file systems have no UUID; the value stays nil.
    if (![url getResourceValue:&uuid forKey:NSURLVolumeUUIDStringKey error:nil] ||
        ![uuid isKindOfClass:[NSString class]]) {
      return {};
    }
    const char* text = [uuid UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
  }
}

auto ReadVolumeLabel(const std::filesystem::path& directory) -> VolumeLabel {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  VolumeLabel   label;
  struct statfs info{};
  if (::statfs(directory.c_str(), &info) == 0) {
    label.mount_point = std::filesystem::path(info.f_mntonname);
  }
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:directory.c_str()];
    if (path == nil) return label;
    NSURL*    url  = [NSURL fileURLWithPath:path isDirectory:YES];
    NSString* name = nil;
    if ([url getResourceValue:&name forKey:NSURLVolumeNameKey error:nil] &&
        [name isKindOfClass:[NSString class]]) {
      const char* text = [name UTF8String];
      if (text != nullptr) {
        label.name = std::string(text);
      }
    }
  }
  return label;
}

auto ListVolumeMountPoints() -> std::vector<VolumeMountPoint> {
  std::vector<VolumeMountPoint> mount_points;
  @autoreleasepool {
    NSArray<NSURL*>* volumes = [[NSFileManager defaultManager]
        mountedVolumeURLsIncludingResourceValuesForKeys:@[ NSURLVolumeUUIDStringKey ]
                                                options:0];
    for (NSURL* volume in volumes) {
      NSString* uuid = nil;
      if (![volume getResourceValue:&uuid forKey:NSURLVolumeUUIDStringKey error:nil] ||
          ![uuid isKindOfClass:[NSString class]]) {
        continue;
      }
      const char* identity = [uuid UTF8String];
      const char* path     = [[volume path] fileSystemRepresentation];
      if (identity == nullptr || path == nullptr) continue;
      mount_points.push_back(VolumeMountPoint{std::string(identity), std::filesystem::path(path)});
    }
  }
  return mount_points;
}

auto ListMountPointPaths() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> mount_points;
  // MNT_NOWAIT returns the cached values of the kernel and asks no file system.
  const int                          count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
  if (count <= 0) return mount_points;
  // A file system that is mounted between the two calls is left for the next read.
  std::vector<struct statfs> entries(static_cast<std::size_t>(count));
  const int                  read = ::getfsstat(
      entries.data(), static_cast<int>(entries.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int index = 0; index < read; ++index) {
    mount_points.emplace_back(entries[static_cast<std::size_t>(index)].f_mntonname);
  }
  return mount_points;
}

}  // namespace alcedo
