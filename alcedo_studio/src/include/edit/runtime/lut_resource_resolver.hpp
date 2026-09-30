//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "edit/operators/models/lut_reference.hpp"

namespace alcedo {

/// Outcome of resolving a LUT reference for one render.
enum class LutResourceStatus : std::uint8_t {
  /// The adjustment references no LUT.
  kNotReferenced,
  /// A file exists for the reference. Its contents are validated when it is parsed.
  kAvailable,
  /// No file exists for the reference. The render skips only this LUT operation.
  kMissing,
};

/**
 * @brief The file a LUT reference currently resolves to, and its content identity.
 *
 * A minimal value returned by a resolver; it is not a copy of a library entry.
 * @ref content_sha256 is the verified digest of an official package file (from the
 * library inventory) and is empty for files the library does not hash. The stamp
 * fields identify the bytes of other files.
 */
struct LutResourceResolution {
  LutResourceStatus     status = LutResourceStatus::kNotReferenced;
  std::filesystem::path path;
  std::uintmax_t        file_size   = 0;
  std::int64_t          write_ticks = 0;
  std::string           content_sha256;

  /// Hash of status, normalized path, stamp, and digest; 0 when not referenced.
  /// Renders compare it with the previous frame to find a changed LUT resource.
  [[nodiscard]] auto    ContentIdentity() const -> std::uint64_t;
};

/**
 * @brief Render-side port that maps a LUT reference to its current file.
 *
 * Implementations: the LUT library (official and library references through its
 * published inventory, legacy file paths through exact paths and recorded previous
 * roots) and @ref FileLutResourceResolver (file paths only).
 *
 * Thread: both methods may be called from any render thread at the same time.
 * They must not throw for an absent file; absence is @ref LutResourceStatus::kMissing.
 */
class LutResourceResolver {
 public:
  virtual ~LutResourceResolver() = default;

  /// Current resolution of @p reference. Reads no file contents.
  [[nodiscard]] virtual auto Resolve(const LutReference& reference) const
      -> LutResourceResolution = 0;

  /**
   * @brief Resolve @p reference and call @p visitor while the resolved file stays in place.
   *
   * The owner does not remove or replace the file until @p visitor returns, so the caller can
   * read and parse it. Do not start GPU work or retain the path inside @p visitor.
   */
  virtual void ReadResource(
      const LutReference&                                      reference,
      const std::function<void(const LutResourceResolution&)>& visitor) const = 0;
};

/**
 * @brief Resolver without a LUT library: a file reference resolves to its exact path.
 *
 * Official and library references resolve to Missing because no library root is known.
 * Used by renderers that were constructed without the application library (tools and tests).
 */
class FileLutResourceResolver final : public LutResourceResolver {
 public:
  [[nodiscard]] auto Resolve(const LutReference& reference) const -> LutResourceResolution override;
  void               ReadResource(
                    const LutReference&                                      reference,
                    const std::function<void(const LutResourceResolution&)>& visitor) const override;
};

/// Shared FileLutResourceResolver used when a renderer receives no resolver.
[[nodiscard]] auto DefaultLutResourceResolver() -> std::shared_ptr<const LutResourceResolver>;

/// Resolution of an existing regular file at @p path with its size and write time,
/// or Missing when no regular file exists there.
[[nodiscard]] auto ResolveLutFile(const std::filesystem::path& path) -> LutResourceResolution;

}  // namespace alcedo
