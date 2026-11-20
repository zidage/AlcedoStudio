//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#import <Foundation/Foundation.h>

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

}  // namespace alcedo
