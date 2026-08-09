//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "app/image_pool_service.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::ui::controllers {

void EnsureLoadingOperatorDefaults(const std::shared_ptr<CPUPipelineExecutor>& exec);
void RebuildBaselinePipelineForImage(const std::shared_ptr<CPUPipelineExecutor>& exec,
                                     const std::shared_ptr<ImagePoolService>&    image_pool,
                                     image_id_t                                  image_id);
void AttachExecutionStages(const std::shared_ptr<CPUPipelineExecutor>& exec,
                           IFrameSink*                                 frame_sink);

}  // namespace alcedo::ui::controllers
