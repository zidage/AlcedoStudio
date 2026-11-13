//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <array>

#include "edit/operators/models/color_wheel_tone.hpp"

// QPointF forms of the tone wheel disc equations in edit/operators/models/color_wheel_tone.hpp.
namespace alcedo::ui::color_wheel {

using alcedo::color_wheel::kDiscMaxStops;
using alcedo::color_wheel::kMasterMaxStops;

auto ClampDiscPoint(const QPointF& p) -> QPointF;
/// Per-channel stops of the disc point @p position at @p strength stops for full radius.
auto DiscToChannelStops(const QPointF& position, float strength) -> std::array<float, 3>;

}  // namespace alcedo::ui::color_wheel
