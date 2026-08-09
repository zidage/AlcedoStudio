//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "app/image_pool_service.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui::controllers {

auto LoadImageInputBuffer(const std::shared_ptr<ImagePoolService>& image_pool, image_id_t image_id)
    -> std::shared_ptr<ImageBuffer>;

}  // namespace alcedo::ui::controllers
