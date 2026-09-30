//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lut_resource_resolver.hpp"

#include <string_view>
#include <system_error>
#include <variant>

#include "edit/runtime/content_key.hpp"

namespace alcedo {

auto LutResourceResolution::ContentIdentity() const -> std::uint64_t {
  if (status == LutResourceStatus::kNotReferenced) {
    return 0;
  }
  ContentHash hash;
  hash.MixU32(static_cast<std::uint32_t>(status));
  const auto utf8 = path.lexically_normal().generic_u8string();
  hash.MixText(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
  hash.MixU64(static_cast<std::uint64_t>(file_size));
  hash.MixU64(static_cast<std::uint64_t>(write_ticks));
  hash.MixText(content_sha256);
  const auto identity = hash.Key().hash;
  // 0 means "not referenced"; keep a referenced resource distinguishable from it.
  return identity == 0 ? 1 : identity;
}

auto ResolveLutFile(const std::filesystem::path& path) -> LutResourceResolution {
  LutResourceResolution resolution;
  resolution.path = path;
  std::error_code error;
  if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
    resolution.status = LutResourceStatus::kMissing;
    return resolution;
  }
  resolution.status      = LutResourceStatus::kAvailable;
  const auto size        = std::filesystem::file_size(path, error);
  resolution.file_size   = error ? 0 : size;
  const auto time        = std::filesystem::last_write_time(path, error);
  resolution.write_ticks = error ? 0 : time.time_since_epoch().count();
  return resolution;
}

auto FileLutResourceResolver::Resolve(const LutReference& reference) const
    -> LutResourceResolution {
  if (IsEmptyLutReference(reference)) {
    return {};
  }
  if (const auto* file = std::get_if<FileLutReference>(&reference)) {
    return ResolveLutFile(std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(file->path.data()), file->path.size())));
  }
  LutResourceResolution missing;
  missing.status = LutResourceStatus::kMissing;
  return missing;
}

void FileLutResourceResolver::ReadResource(
    const LutReference&                                      reference,
    const std::function<void(const LutResourceResolution&)>& visitor) const {
  visitor(Resolve(reference));
}

auto DefaultLutResourceResolver() -> std::shared_ptr<const LutResourceResolver> {
  static const auto resolver = std::make_shared<const FileLutResourceResolver>();
  return resolver;
}

}  // namespace alcedo
