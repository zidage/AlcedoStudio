//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/mask_asset_reader.hpp"

#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

class MissingMaskAssetStore final : public MaskAssetReader {
 public:
  [[nodiscard]] auto Read(const MaskAssetKey& key) const -> MaskAsset override {
    throw std::runtime_error("Mask asset " + std::string{key.Value()} +
                             " cannot be read: this renderer has no project mask asset store");
  }
};

}  // namespace

auto DefaultMaskAssetReader() -> std::shared_ptr<const MaskAssetReader> {
  static const auto reader = std::make_shared<const MissingMaskAssetStore>();
  return reader;
}

}  // namespace alcedo
