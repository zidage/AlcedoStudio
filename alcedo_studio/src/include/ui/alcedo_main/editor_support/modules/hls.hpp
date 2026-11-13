//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>

#include "ui/alcedo_main/editor_support/modules/hls_math.hpp"

namespace alcedo::ui::hls {

auto CandidateColor(float hue_degrees) -> QColor;

}  // namespace alcedo::ui::hls
