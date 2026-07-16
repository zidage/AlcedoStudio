//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

/// Narrow status surface shared by library/project modules.
/// Controllers report transient user-facing status without depending on the host.
class IUiStatusSink {
 public:
  virtual ~IUiStatusSink() = default;

  virtual void SetServiceMessage(const i18n::LocalizedText& message) = 0;
  virtual void SetTaskState(const i18n::LocalizedText& status, int progress,
                            bool cancelVisible)                      = 0;
  virtual void ScheduleIdleTaskStateReset(int delayMs)               = 0;
};

}  // namespace alcedo::ui
