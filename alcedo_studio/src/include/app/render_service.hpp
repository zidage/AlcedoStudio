//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <vector>

#include "renderer/pipeline_scheduler.hpp"

namespace alcedo {
class RenderService {
 public:
  static auto GetPreviewScheduler() -> std::shared_ptr<PipelineScheduler> {
    return std::make_shared<PipelineScheduler>();
  }

  /// Shared workers whose only user is ExportService. ThumbnailService owns its own scheduler and
  /// batch executors.
  static auto GetThumbnailOrExportScheduler() -> std::shared_ptr<PipelineScheduler> {
    static size_t thread_count = fmax(size_t(2), std::thread::hardware_concurrency() / 2);
    static std::shared_ptr<PipelineScheduler> scheduler =
        std::make_shared<PipelineScheduler>(thread_count);
    return scheduler;
  }

  static auto GetThumbnailOrExportScheduler(size_t thread_cnt)
      -> std::shared_ptr<PipelineScheduler> {
    return std::make_shared<PipelineScheduler>(thread_cnt);
  }
};
};  // namespace alcedo