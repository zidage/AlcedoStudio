//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace alcedo::ui {

inline constexpr const char* kMaskThumbnailImageProviderId = "alcedo-mask-thumb";

/**
 * @brief Short-lived display table of accepted Mask thumbnail QImages.
 *
 * Keys are UI request ids, not content keys. The project LRU is
 * @ref alcedo::MaskThumbnailService. QImage implicit sharing keeps pixels alive
 * after LRU eviction.
 */
class MaskThumbnailImageStore {
 public:
  [[nodiscard]] auto Put(std::uint64_t request_id, QImage image) -> QString;
  void               Remove(std::uint64_t request_id);
  [[nodiscard]] auto Get(std::uint64_t request_id) const -> QImage;
  [[nodiscard]] static auto MakeUrl(std::uint64_t request_id) -> QString;
  [[nodiscard]] static auto ParseProviderId(const QString& id, std::uint64_t* request_id) -> bool;

 private:
  mutable std::mutex                            mutex_;
  std::unordered_map<std::uint64_t, QImage>     images_;
};

class MaskThumbnailImageProvider final : public QQuickImageProvider {
 public:
  explicit MaskThumbnailImageProvider(std::shared_ptr<MaskThumbnailImageStore> store);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<MaskThumbnailImageStore> store_;
};

[[nodiscard]] auto SharedMaskThumbnailImageStore() -> std::shared_ptr<MaskThumbnailImageStore>;

}  // namespace alcedo::ui
