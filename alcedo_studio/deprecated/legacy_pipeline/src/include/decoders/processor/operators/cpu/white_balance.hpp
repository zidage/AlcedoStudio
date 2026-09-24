//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <opencv2/core.hpp>

namespace alcedo {
namespace CPU {
void ToLinearRef(cv::Mat& img, LibRaw& raw_processor);
};
};  // namespace alcedo