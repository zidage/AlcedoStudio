//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exiv2/xmp_exiv2.hpp>
#include <mutex>

namespace alcedo {

/// Initializes the Exiv2 XMP toolkit for the process lifetime and terminates it on destruction.
///
/// `Exiv2::XmpParser::decode` initializes the toolkit lazily on the first thread that reads XMP,
/// and that initialization registers namespaces in a global map without synchronization. Thumbnail
/// and import workers read XMP in parallel, so the host creates one instance on the main thread
/// before any worker starts. The mutex serializes the later namespace registrations that
/// `XmpParser::decode` performs for unknown namespaces on those workers.
class Exiv2XmpToolkitScope {
 public:
  Exiv2XmpToolkitScope()
      : initialized_(Exiv2::XmpParser::initialize(&LockNamespaceRegistry, &namespace_mutex_)) {}

  ~Exiv2XmpToolkitScope() {
    if (initialized_) {
      Exiv2::XmpParser::terminate();
    }
  }

  Exiv2XmpToolkitScope(const Exiv2XmpToolkitScope&)                                  = delete;
  auto               operator=(const Exiv2XmpToolkitScope&) -> Exiv2XmpToolkitScope& = delete;

  /// False when the XMP toolkit cannot start; the host must not read metadata in that case.
  [[nodiscard]] auto initialized() const -> bool { return initialized_; }

 private:
  static void LockNamespaceRegistry(void* mutex, bool lock) {
    auto* namespace_mutex = static_cast<std::mutex*>(mutex);
    if (lock) {
      namespace_mutex->lock();
    } else {
      namespace_mutex->unlock();
    }
  }

  std::mutex namespace_mutex_;
  bool       initialized_;
};

}  // namespace alcedo
